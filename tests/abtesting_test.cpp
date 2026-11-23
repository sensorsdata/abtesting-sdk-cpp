// 使用真实埋点 SDK 的 PropertiesNode、Track 与事件队列，仅替换 Post。
// 引入实现以测试内部 Consumer，不增加客户可见的测试接口。
#include "../src/sensors_abtesting_sdk.cpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <sys/stat.h>
#include <utime.h>

using namespace std;
using json = sensors_json::json;
using sensors_abtesting::DefaultConsumer;
using sensors_abtesting::ABTestPropertiesNode;
using namespace sensors_abtesting::abtesting_utils;
using Analytics = sensors_analytics::Sdk;
using sensors_analytics::Response;

#define CHECK(condition) do { if (!(condition)) { \
  throw runtime_error(string(__FILE__) + ":" + to_string(__LINE__) + ": " #condition); \
} } while (false)

namespace {
mutex network_mtx;
function<Response(const json &)> responder;
vector<json> requests;
string work_dir;
string event_file;
int file_index = 0;

Response Reply(const json &value) {
  return Response{200, value.dump(), {}};
}

void SetResponder(function<Response(const json &)> callback) {
  lock_guard<mutex> lock(network_mtx);
  responder = callback;
}

void SetReply(const json &value) {
  SetResponder([value](const json &) { return Reply(value); });
}

size_t RequestCount() {
  lock_guard<mutex> lock(network_mtx);
  return requests.size();
}

json ReadJson(const string &file_path) {
  ifstream file(file_path);
  CHECK(file.good());
  const string value((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
  return json::parse(value);
}

void WriteJson(const string &file_path, const json &value) {
  ofstream file(file_path);
  file << value.dump() << endl;
  CHECK(file.good());
}

vector<json> Events() {
  // 读取真实埋点 SDK 的本地事件队列，包含实际属性校验后的数据。
  Analytics::DumpAllRecordsToDisk();
  vector<json> result;
  ifstream file(event_file);
  string line;
  while (getline(file, line)) {
    if (!line.empty()) {
      json record = json::parse(line);
      CHECK(record.at("event") == "$ABTestTrigger");
      result.push_back(record);
    }
  }
  file.close();
  Analytics::ClearQueue();
  return result;
}

json Experiment(string result = "A", string group = "1", string id = "100",
                string param = "color", string value = "red") {
  return {{"abtest_experiment_id", id}, {"abtest_experiment_group_id", group},
          {"abtest_experiment_result_id", result}, {"abtest_experiment_version", "1"},
          {"subject_name", "USER"}, {"subject_id", "subject_a"},
          {"experiment_type", "CODE"}, {"is_white_list", false},
          {"is_control_group", false},
          {"variables", json::array({{{"name", param}, {"type", "STRING"}, {"value", value}}})}};
}

json Out(string id = "100", string param = "color") {
  return {{"abtest_experiment_id", id}, {"abtest_experiment_result_id", "-1"},
          {"subject_name", "USER"}, {"subject_id", "subject_a"},
          {"variables", json::array({{{"name", param}}})}};
}

json ResponseData(const json &results = json::array(), const json &out = json::array()) {
  return {{"status", "SUCCESS"}, {"results", results}, {"out_list", out}};
}

json One(const json &item) { return ResponseData(json::array({item})); }

json SampleResponse() {
  auto platform = Experiment("10110103", "3", "1011", "os", "windows");
  platform["subject_name"] = "CUSTOM";

  auto allowlisted = Experiment("10120101", "1", "1012", "theme_color", "red");
  allowlisted["subject_name"] = "DEVICE";
  allowlisted["is_white_list"] = true;
  allowlisted["variables"].push_back({{"name", "theme_index"}, {"type", "INTEGER"}, {"value", "1"}});

  auto typed = Experiment("10070100", "0", "1007");
  typed["is_control_group"] = true;
  const json page_config = {{"title", "Hello World"}, {"enabled", true}};
  typed["variables"] = json::array({
      {{"name", "sample_count"}, {"type", "INTEGER"}, {"value", "1"}},
      {{"name", "enable_feature"}, {"type", "BOOLEAN"}, {"value", "true"}},
      {{"name", "page_config"}, {"type", "JSON"}, {"value", page_config.dump()}}});

  auto response = ResponseData(json::array({platform, allowlisted, typed}),
                               json::array({Out("335", "os")}));
  response["track_config"] = {{"item_switch", false}, {"trigger_switch", true},
      {"property_set_switch", false},
      {"trigger_content_ext", json::array({"abtest_experiment_version", "abtest_experiment_result_id"})}};
  return response;
}

json Get(future<json> value) {
  CHECK(value.wait_for(chrono::seconds(5)) == future_status::ready);
  return value.get();
}

struct Harness {
  string path;
  unique_ptr<DefaultConsumer> sdk;

  explicit Harness(const json &disk = json(), bool writable = true) {
    path = work_dir + "/cache_" + to_string(++file_index) + ".json";
    if (!writable) {
      path += "/missing/cache.json";
    } else if (!disk.is_null()) {
      WriteJson(path, disk);
    }
    Restart();
  }
  void Restart() {
    sdk.reset();
    sdk.reset(new DefaultConsumer("https://ab.example.invalid", "project", path, false));
  }
  json Cache(const string &param = "color", const json &fallback = "default") {
    return sdk->FetchExperimentResult(kFetchModeCache, param, fallback,
                                      ABTestPropertiesNode(), FetchCompletionFunc, 1000);
  }
  future<json> Start(const string &param = "color", const json &fallback = "default",
                     const ABTestPropertiesNode &properties = ABTestPropertiesNode(),
                     FetchMode mode = kFetchModeAsync) {
    auto promise_value = make_shared<promise<json>>();
    future<json> value = promise_value->get_future();
    sdk->FetchExperimentResult(mode, param, fallback, properties,
        [promise_value](const json &result) { promise_value->set_value(result); }, 1000);
    return value;
  }
  void Load(const json &response) {
    SetReply(response);
    CHECK(Get(Start("__refresh__", 0)) == 0);
  }
  void User(const string &id, bool is_login = true) {
    Analytics::Identify(id, is_login);
    sdk->observer_->Update();
  }
};

string ResultId(const json &event) {
  return event.at("properties").at("$abtest_experiment_result_id").get<string>();
}

json SavedHit(const Harness &h, const string &id = "100") {
  const json disk = ReadJson(h.path + ".hit_records.json");
  for (const auto &group : disk.at("hit_records")) {
    if (group.at("results").count(id)) {
      return group.at("results").at(id);
    }
  }
  throw runtime_error("missing saved hit " + id);
}

void TestFixture() {
  Harness h;
  h.Load(SampleResponse());
  CHECK(Events().empty());
  CHECK(h.Cache("os") == "windows");
  auto events = Events();
  CHECK(events.size() == 2);
  CHECK(events[0].at("properties").at("$abtest_experiment_id") == "335");
  CHECK(ResultId(events[0]) == "-1");
  CHECK(!events[0].at("properties").count("$abtest_experiment_group_id"));
  CHECK(!events[0].at("properties").count("$abtest_experiment_version"));
  CHECK(events[0].at("properties").at("$lib_plugin_version")[0] == "cpp_abtesting:1.0.0");
  CHECK(ResultId(events[1]) == "10110103");
  CHECK(!events[1].at("properties").count("$lib_plugin_version"));
  CHECK(events[1].at("properties").at("$abtest_experiment_version") == "1");
  CHECK(!events[1].at("properties").count("$subject_id"));
  CHECK(events[1].at("distinct_id") == "user_a");
  CHECK(h.Cache("os") == "windows");
  CHECK(Events().empty());
  CHECK(h.Cache("sample_count", 0) == 1);
  CHECK(h.Cache("enable_feature", false) == true);
  CHECK(h.Cache("page_config", json::object()).at("title") == "Hello World");
  CHECK(Events().size() == 1);  // 三个参数同一试验只触发一次。
  CHECK(h.Cache("theme_color") == "red");
  CHECK(h.Cache("theme_index", 0) == 1);
  CHECK(Events().empty());  // 白名单只返回值。
  CHECK(ReadJson(h.path + ".hit_records.json").at("hit_records").size() == 2);
}

void TestTransitions() {
  Harness h;
  for (const string &value : vector<string>{"A", "A", "B", "A"}) {
    h.Load(One(Experiment(value)));
    CHECK(h.Cache() == "red");
  }
  auto events = Events();
  CHECK(events.size() == 3);
  CHECK(ResultId(events[0]) == "A" && ResultId(events[1]) == "B" && ResultId(events[2]) == "A");
  auto next = Experiment("A2");
  next["abtest_experiment_version"] = "2";
  h.Load(One(next));
  CHECK(h.Cache() == "red");
  events = Events();
  CHECK(events.size() == 1);
  CHECK(ResultId(events[0]) == "A2");
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "A2");
}

void TestOutAndReentry() {
  Harness h;
  const auto out = ResponseData(json::array(), json::array({Out()}));
  h.Load(out);
  CHECK(h.Cache() == "default");
  CHECK(h.Cache() == "default");
  CHECK(Events().size() == 1);  // 没有历史也处理后端明确的出组。
  h.Restart();
  CHECK(h.Cache() == "default");
  CHECK(Events().empty());  // 最小出组记录和 -1 历史均可重启恢复。
  h.Load(One(Experiment()));
  CHECK(h.Cache() == "red");
  h.Load(out);
  CHECK(h.Cache() == "default");
  h.Load(One(Experiment()));
  CHECK(h.Cache() == "red");
  const auto events = Events();
  CHECK(events.size() == 3);
  CHECK(ResultId(events[0]) == "A" && ResultId(events[1]) == "-1" && ResultId(events[2]) == "A");
}

void TestOutOrdering() {
  Harness h;
  h.Load(ResponseData(json::array({Experiment("B", "2", "200")}),
                       json::array({Out("100"), Out("101")})));
  CHECK(h.Cache() == "red");
  const auto events = Events();
  CHECK(events.size() == 3);
  CHECK(events[0].at("properties").at("$abtest_experiment_id") == "100");
  CHECK(events[1].at("properties").at("$abtest_experiment_id") == "101");
  CHECK(ResultId(events[2]) == "B");
  h.Load(ResponseData(json::array({Experiment("C")}), json::array({Out()})));
  CHECK(h.Cache() == "red");
  const auto conflict = Events();
  CHECK(conflict.size() == 1 && ResultId(conflict[0]) == "C");
}

void TestSubjects() {
  Harness h;
  json item = Experiment();
  h.Load(One(item)); h.Cache();
  h.User("user_b", false);
  CHECK(h.Cache() == "default");
  h.Load(One(item)); h.Cache();  // 请求身份改变，相同主体不多发。
  CHECK(Events().size() == 1);
  item["subject_name"] = "DEVICE";
  h.Load(One(item)); h.Cache();
  item["subject_id"] = "subject_b";
  h.Load(One(item)); h.Cache();
  item["abtest_experiment_id"] = "101";
  h.Load(One(item)); h.Cache();
  CHECK(Events().size() == 3);
  item["subject_name"] = "CUSTOM";
  h.Load(One(item)); h.Cache();
  CHECK(Events().size() == 1);
  CHECK(ReadJson(h.path + ".hit_records.json").at("hit_records").size() == 4);
}

void TestPersistence() {
  Harness h;
  const auto response = SampleResponse();
  h.Load(response);
  CHECK(h.Cache("page_config", json::object()).at("title") == "Hello World");
  CHECK(Events().size() == 1);
  CHECK(SavedHit(h, "1007").at("abtest_experiment_result_id") == "10070100");
  CHECK(h.Cache("os") == "windows");
  CHECK(Events().size() == 2);
  h.Restart();
  CHECK(Events().empty());
  CHECK(h.Cache("page_config", json::object()).at("title") == "Hello World");
  CHECK(h.Cache("os") == "windows");
  CHECK(Events().empty());
  h.Load(response);  // 全量网络替换不能丢失本地命中历史。
  CHECK(SavedHit(h, "1007").at("abtest_experiment_result_id") == "10070100");
  CHECK(h.Cache("page_config", json::object()).at("enabled") == true);
  CHECK(Events().empty());
  auto untrusted = response;
  untrusted["hit_records"] = json::array();
  h.Load(untrusted);
  CHECK(h.Cache("os") == "windows");
  CHECK(Events().empty());  // 同名后端字段不能重置本地历史。
}

void TestSwitchAndRestart() {
  Harness h;
  auto value = One(Experiment());
  value["track_config"] = {{"trigger_switch", false}};
  h.Load(value); h.Cache();
  CHECK(Events().empty());
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "A");
  h.Restart(); h.Cache();
  CHECK(Events().empty());
  value["track_config"] = {{"trigger_switch", true},
      {"trigger_content_ext", json::array({"abtest_experiment_result_id"})}};
  h.Load(value); h.Cache();
  CHECK(Events().empty());  // 关闭时已记录，开启后同结果不补发。
  value["results"][0]["abtest_experiment_result_id"] = "B";
  h.Load(value); h.Cache();
  auto events = Events();
  CHECK(events.size() == 1 && ResultId(events[0]) == "B");
  CHECK(events[0].at("properties").count("$lib_plugin_version"));
}

void TestConfigurations() {
  Harness h;
  vector<json> configs = {
      json(), json::object(), {{"trigger_switch", true}},
      {{"trigger_switch", true}, {"trigger_content_ext", json::array()}},
      {{"trigger_content_ext", json::array({"abtest_experiment_result_id"})}},
      {{"trigger_switch", "invalid"}}, {{"trigger_switch", true}, {"trigger_content_ext", 1}},
      json::array()};
  for (size_t index = 0; index < configs.size(); ++index) {
    auto value = One(Experiment(to_string(index)));
    if (!configs[index].is_null()) value["track_config"] = configs[index];
    h.Load(value); h.Cache();
    auto events = Events();
    if (index == 4) {
      CHECK(events.empty());
    } else {
      CHECK(events.size() == 1);
      const bool has_ext = events[0].at("properties").count("$abtest_experiment_result_id") != 0;
      CHECK(has_ext == (index != 2 && index != 3));
    }
  }
  // 后续成功响应无配置应恢复默认，而非沿用前一远程配置。
  auto value = One(Experiment("off"));
  value["track_config"] = {{"trigger_switch", false}};
  h.Load(value); h.Cache(); CHECK(Events().empty());
  h.Load(One(Experiment("default"))); h.Cache();
  const auto events = Events();
  CHECK(events.size() == 1 && ResultId(events[0]) == "default");
}

void TestExtensions() {
  Harness h;
  auto item = Experiment();
  item["label"] = "hello"; item["enabled"] = true; item["count"] = 7;
  item["ratio"] = 2.5; item["tags"] = json::array({"a", "b"});
  item["object"] = {{"nested", true}}; item["mixed"] = json::array({"a", 1});
  item["nothing"] = nullptr; item["lib_plugin_version"] = "wrong";
  item["distinct_id"] = "wrong"; item["too_big"] = uint64_t(-1);
  auto value = One(item);
  value["track_config"] = {{"trigger_switch", true}, {"trigger_content_ext",
      json::array({"label", "enabled", "count", "ratio", "tags", "object", "mixed",
                   "nothing", "missing", "bad-key", "$label", "lib_plugin_version",
                   "abtest_experiment_id", "distinct_id", "too_big"})}};
  h.Load(value); h.Cache();
  const auto events = Events();
  CHECK(events.size() == 1);
  const auto &props = events[0].at("properties");
  CHECK(props.at("$label") == "hello" && props.at("$enabled") == true);
  CHECK(props.at("$count") == 7 && props.at("$ratio") == 2.5);
  CHECK(props.at("$tags") == json::array({"a", "b"}));
  CHECK(!props.count("$object") && !props.count("$mixed") && !props.count("$nothing"));
  CHECK(!props.count("$missing") && !props.count("$bad-key") && !props.count("$$label"));
  CHECK(!props.count("$distinct_id") && !props.count("$too_big"));
  CHECK(props.at("$abtest_experiment_id") == "100");
  CHECK(props.at("$lib_plugin_version")[0] == "cpp_abtesting:1.0.0");
  CHECK(!props.count("$abtest_experiment_result_id"));
}

void TestInvalidRecords() {
  Harness h;
  auto broken = Experiment("broken", "1", "999");
  broken.erase("subject_id");
  auto empty = Experiment("empty"); empty["subject_id"] = "";
  auto wrong = Experiment("wrong"); wrong["subject_id"] = 123;
  auto visual = Experiment(); visual["experiment_type"] = "VISUAL";
  h.Load(ResponseData(json::array({broken, empty, wrong, visual, Experiment()})));
  CHECK(h.Cache() == "red");
  CHECK(Events().size() == 1);
  CHECK(!ReadJson(h.path + ".hit_records.json").at("hit_records")[0].at("results").count("999"));
  auto bad_id = Experiment(); bad_id["abtest_experiment_id"] = 1.5;
  SetReply(One(bad_id));
  CHECK(Get(h.Start()) == "default");
  CHECK(h.Cache() == "red");
  CHECK(Events().empty());
  auto numeric = Experiment("B");
  numeric["abtest_experiment_id"] = uint64_t(-1);
  numeric["abtest_experiment_group_id"] = 123;
  h.Load(One(numeric)); h.Cache();
  const auto events = Events();
  CHECK(events.size() == 1);
  CHECK(events[0].at("properties").at("$abtest_experiment_id") == "18446744073709551615");
  CHECK(events[0].at("properties").at("$abtest_experiment_group_id") == "123");
}

void TestLegacy() {
  Harness h;
  auto item = Experiment();
  item.erase("subject_name"); item.erase("subject_id"); item.erase("abtest_experiment_result_id");
  h.Load(One(item)); h.Cache(); h.Cache();
  CHECK(Events().size() == 1);
  CHECK(ReadJson(h.path + ".hit_records.json").at("hit_records")[0].at("legacy_user").at("is_login_id") == true);
  item["abtest_experiment_group_id"] = "2";
  h.Load(One(item)); h.Cache(); CHECK(Events().size() == 1);
  item["abtest_experiment_result_id"] = "A";
  h.Load(One(item)); h.Cache(); CHECK(Events().empty());
  CHECK(!SavedHit(h).count("abtest_experiment_result_id"));  // 补齐未触发事件，不写盘。
  item["abtest_experiment_result_id"] = "B";
  h.Load(One(item)); h.Cache(); CHECK(Events().size() == 1);
  item.erase("abtest_experiment_result_id");
  h.Load(One(item)); h.Cache(); CHECK(Events().empty());
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "B");
  h.Restart(); h.Cache(); CHECK(Events().empty());
  h.User("user_a", false); h.Load(One(item)); h.Cache();
  CHECK(Events().size() == 1);  // 同文本 ID，登录/匿名的旧协议分组仍分离。
}

void TestFailuresAndEmpty() {
  Harness h;
  h.Load(One(Experiment())); h.Cache(); Events();
  vector<Response> failures = {
      {500, "server error", {}}, {200, "", {}}, {200, "{", {}},
      Reply({{"status", "FAILED"}}), Reply({{"status", "SUCCESS"}, {"results", json::object()}})};
  for (const auto &response : failures) {
    SetResponder([response](const json &) { return response; });
    CHECK(Get(h.Start()) == "default");
    CHECK(h.Cache() == "red");
    CHECK(Events().empty());
  }
  h.Load(ResponseData());
  CHECK(h.Cache() == "default");
  CHECK(Events().empty());  // 不能从列表消失推断出组。
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "A");
}

void TestInvalidValuesAndOut() {
  Harness h;
  auto value = Experiment();
  const vector<pair<string,string>> cases = {
      {"INTEGER", "999999999999999999999999"}, {"INTEGER", "1x"},
      {"BOOLEAN", "yes"}, {"JSON", "{"}, {"JSON", "[]"}};
  for (const auto &test : cases) {
    value["variables"][0]["type"] = test.first;
    value["variables"][0]["value"] = test.second;
    h.Load(One(value));
    json fallback = test.first == "INTEGER" ? json(0) :
                    test.first == "BOOLEAN" ? json(false) : json::object();
    CHECK(h.Cache("color", fallback) == fallback);
    CHECK(Events().empty());
  }
  h.Load(ResponseData(json::array({Experiment("B", "1", "200")}), json::array({Out()})));
  CHECK(h.Cache("color", 0) == 0);  // 类型不匹配不影响出组。
  const auto events = Events();
  CHECK(events.size() == 1 && ResultId(events[0]) == "-1");
}

void TestFetchModesAndCallback() {
  Harness h;
  h.Load(One(Experiment()));
  size_t before = RequestCount();
  CHECK(Get(h.Start("color", "default", ABTestPropertiesNode(), kFetchModeFast)) == "red");
  CHECK(RequestCount() == before);
  CHECK(Events().size() == 1);
  h.Load(ResponseData(json::array(), json::array({Out()})));
  before = RequestCount();
  CHECK(Get(h.Start("color", "default", ABTestPropertiesNode(), kFetchModeFast)) == "default");
  CHECK(RequestCount() == before + 1);
  CHECK(Events().size() == 1);  // 缓存和网络两阶段不重复出组。

  SetReply(One(Experiment("B")));
  auto completion = make_shared<promise<void>>();
  auto done = completion->get_future();
  auto calls = make_shared<atomic<int>>(0);
  auto error = make_shared<exception_ptr>();
  h.sdk->FetchExperimentResult(kFetchModeAsync, "color", "default", ABTestPropertiesNode(),
      [&h, completion, calls, error](const json &result) {
        ++*calls;
        try {
          CHECK(result == "red");
          CHECK(Events().size() == 1);  // 事件先于业务回调入队。
          CHECK(h.Cache() == "red");  // 回调重入不死锁。
        } catch (...) { *error = current_exception(); }
        completion->set_value();
        throw runtime_error("customer handler exception");
      }, 1000);
  CHECK(done.wait_for(chrono::seconds(5)) == future_status::ready);
  h.sdk.reset();  // 等待任务退出，检查没有二次回调。
  CHECK(calls->load() == 1);
  if (*error) rethrow_exception(*error);
}

void TestConcurrentHits() {
  Harness h;
  h.Load(One(Experiment()));
  atomic<int> failures(0);
  vector<thread> threads;
  for (int i = 0; i < 32; ++i) {
    threads.emplace_back([&h, &failures]() {
      if (h.Cache() != "red") ++failures;
    });
  }
  for (auto &value : threads) value.join();
  CHECK(failures.load() == 0);
  CHECK(Events().size() == 1);
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "A");
}

struct Gate {
  promise<void> entered;
  promise<void> release;
  shared_future<void> released = release.get_future().share();
  void Wait() {
    entered.set_value();
    if (released.wait_for(chrono::seconds(5)) != future_status::ready) {
      throw runtime_error("network gate timed out");
    }
  }
};

void TestResponseOrdering() {
  Harness h;
  auto gate = make_shared<Gate>();
  auto entered = gate->entered.get_future();
  SetResponder([gate](const json &request) {
    if (request.at("custom_properties").at("request") == "slow") {
      gate->Wait();
      return Reply(One(Experiment("A", "1", "100", "color", "old")));
    }
    return Reply(One(Experiment("B", "2", "100", "color", "new")));
  });
  ABTestPropertiesNode slow, fast;
  slow.SetString("request", "slow"); fast.SetString("request", "fast");
  auto first = h.Start("color", "default", slow);
  CHECK(entered.wait_for(chrono::seconds(5)) == future_status::ready);
  CHECK(Get(h.Start("color", "default", fast)) == "new");
  gate->release.set_value();
  CHECK(Get(std::move(first)) == "old");  // 同身份迟到 SUCCESS 不返回默认值。
  const auto events = Events();
  CHECK(events.size() == 2 && ResultId(events[0]) == "B" && ResultId(events[1]) == "A");
  CHECK(h.Cache() == "old");
  CHECK(Events().empty());
}

void TestIdentityGeneration() {
  Harness h;
  auto gate = make_shared<Gate>();
  auto entered = gate->entered.get_future();
  SetResponder([gate](const json &) { gate->Wait(); return Reply(One(Experiment())); });
  auto old = h.Start();
  CHECK(entered.wait_for(chrono::seconds(5)) == future_status::ready);
  h.User("user_b"); h.User("user_a");  // A → B → A，不能仅比较 ID 文本。
  CHECK(h.Cache() == "default");
  gate->release.set_value();
  CHECK(Get(std::move(old)) == "default");
  CHECK(Events().empty());
  h.Load(One(Experiment())); CHECK(h.Cache() == "red");
  CHECK(Events().size() == 1);
}

void TestCacheRecoveryAndWriteFailure() {
  auto disk = One(Experiment()); disk["user_id"] = "user_a";
  disk.erase("out_list");
  Harness old(disk);  // v0.0.3 原响应文件，无 is_login_id/hit_records。
  CHECK(old.Cache() == "red"); CHECK(Events().size() == 1);
  CHECK(!ReadJson(old.path).count("is_login_id"));  // 缓存命中不重写分流文件。
  old.Restart(); CHECK(old.Cache() == "red"); CHECK(Events().empty());

  auto saved = ReadJson(old.path);
  saved["hit_records"] = ReadJson(old.path + ".hit_records.json").at("hit_records");
  saved["hit_records"].push_back({{"subject_name", "USER"}});  // 损坏分组不丢有效历史。
  Harness partial(saved); CHECK(partial.Cache() == "red"); CHECK(Events().empty());
  saved["hit_records"] = "broken";
  Harness broken(saved); CHECK(broken.Cache() == "red"); CHECK(Events().size() == 1);
  saved["is_login_id"] = false;
  Harness wrong_identity(saved); CHECK(wrong_identity.Cache() == "default");
  CHECK(Events().empty());

  Harness damaged;
  damaged.sdk.reset();
  { ofstream file(damaged.path); file << "{"; }
  damaged.Restart();
  CHECK(damaged.Cache() == "default");
  damaged.Load(One(Experiment())); CHECK(damaged.Cache() == "red");
  CHECK(Events().size() == 1);

  Harness unwritable(json(), false);
  unwritable.Load(One(Experiment()));
  CHECK(unwritable.Cache() == "red"); CHECK(unwritable.Cache() == "red");
  CHECK(Events().size() == 1);  // 写入失败不影响当前进程内返回和去重。
}

void MarkFile(const string &path) {
  struct utimbuf times = {946684800, 946684800};
  CHECK(utime(path.c_str(), &times) == 0);
}

bool StillMarked(const string &path) {
  struct stat info;
  CHECK(stat(path.c_str(), &info) == 0);
  return info.st_mtime == 946684800;
}

void TestSeparateFiles() {
  Harness h;
  const string hits = h.path + ".hit_records.json";
  h.Load(One(Experiment()));
  CHECK(!ReadJson(h.path).count("hit_records"));
  CHECK(!ifstream(hits).good());  // 后台刷新没有命中，不创建历史文件。
  MarkFile(h.path);
  h.Cache(); CHECK(Events().size() == 1);
  CHECK(StillMarked(h.path));  // 触发事件只写命中历史。
  CHECK(ReadJson(hits).size() == 1 && ReadJson(hits).count("hit_records"));
  MarkFile(hits);
  h.Cache(); CHECK(Events().empty());
  CHECK(StillMarked(hits) && StillMarked(h.path));
  h.Load(One(Experiment()));
  CHECK(!StillMarked(h.path));  // 即使响应相同，成功请求仍刷新结果文件。
  CHECK(StillMarked(hits));
  auto changed = Experiment("B", "2");
  h.Load(One(changed));
  CHECK(StillMarked(hits));  // 新结果未被业务查询时不写历史。
  MarkFile(h.path);
  h.Cache(); CHECK(Events().size() == 1);
  CHECK(StillMarked(h.path) && !StillMarked(hits));
  MarkFile(hits);
  auto disabled = One(Experiment("C", "3"));
  disabled["track_config"] = {{"trigger_switch", false}};
  h.Load(disabled); h.Cache(); CHECK(Events().empty());
  CHECK(!StillMarked(hits));
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "C");
  MarkFile(hits);
  MarkFile(h.path);
  SetResponder([](const json &) { return Response{500, "failure", {}}; });
  CHECK(Get(h.Start()) == "default");
  CHECK(StillMarked(h.path) && StillMarked(hits));
  h.Restart();
  CHECK(StillMarked(h.path) && StillMarked(hits));
}

void TestSeparateRecovery() {
  Harness h;
  h.Load(One(Experiment())); h.Cache(); CHECK(Events().size() == 1);
  const string hits = h.path + ".hit_records.json";
  h.sdk.reset();
  { ofstream file(h.path); file << "{"; }
  h.Restart();
  CHECK(h.Cache() == "default");
  h.Load(One(Experiment())); h.Cache(); CHECK(Events().empty());
  h.sdk.reset();
  { ofstream file(hits); file << "{"; }
  h.Restart();
  CHECK(h.Cache() == "red"); CHECK(Events().size() == 1);
  // 独立历史优先，服务端或遗留结果文件中的同名字段不能覆盖它。
  auto combined = ReadJson(h.path);
  combined["hit_records"] = json::array();
  WriteJson(h.path, combined);
  h.Restart(); h.Cache(); CHECK(Events().empty());
  // 旧合并文件首次启动迁移，刷新后移除结果文件中的命中字段。
  combined["hit_records"] = ReadJson(hits).at("hit_records");
  Harness migrated(combined);
  CHECK(ifstream(migrated.path + ".hit_records.json").good());
  CHECK(!ReadJson(migrated.path).count("hit_records"));
  migrated.Load(One(Experiment()));
  CHECK(!ReadJson(migrated.path).count("hit_records"));
  migrated.Restart(); migrated.Cache(); CHECK(Events().empty());
}

void TestTrackConfigStorage() {
  Harness h;
  const string config = h.path + ".track_config.json";
  const string hits = h.path + ".hit_records.json";
  auto response = One(Experiment());
  response["track_config"] = {{"trigger_switch", false},
      {"trigger_content_ext", json::array({"abtest_experiment_result_id"})}};
  h.Load(response);
  CHECK(!ReadJson(h.path).count("track_config"));
  CHECK(ReadJson(config).at("track_config") == response.at("track_config"));
  CHECK(!ifstream(hits).good());
  MarkFile(config);
  h.Load(response);
  CHECK(!StillMarked(config));  // 与 iOS 一致：有效远程配置每次成功请求都保存。
  MarkFile(config); MarkFile(h.path);
  h.Cache(); CHECK(Events().empty());
  CHECK(StillMarked(config) && StillMarked(h.path));
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "A");
  h.Restart(); h.Cache(); CHECK(Events().empty());
  // 结果文件损坏不影响独立配置和命中历史恢复。
  h.sdk.reset();
  { ofstream file(h.path); file << "{"; }
  h.Restart(); CHECK(h.Cache() == "default");
  auto result_only = One(Experiment("B"));
  result_only["user_id"] = "user_a"; result_only["is_login_id"] = true;
  WriteJson(h.path, result_only);
  h.Restart(); h.Cache(); CHECK(Events().empty());
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "B");
  // 配置损坏时使用默认值，不丢弃仍有效的结果和历史。
  h.sdk.reset();
  { ofstream file(config); file << "{"; }
  result_only["results"][0]["abtest_experiment_result_id"] = "C";
  WriteJson(h.path, result_only);
  h.Restart(); h.Cache(); CHECK(Events().size() == 1);
  // 独立配置优先于遗留响应中的配置。
  h.Load(response);
  result_only["results"][0]["abtest_experiment_result_id"] = "D";
  result_only["track_config"] = {{"trigger_switch", true}};
  WriteJson(h.path, result_only);
  h.Restart(); h.Cache(); CHECK(Events().empty());
  CHECK(SavedHit(h).at("abtest_experiment_result_id") == "D");
  // 请求失败保留配置文件；缺少、空对象及非法配置清除文件、恢复默认。
  MarkFile(config);
  SetResponder([](const json &) { return Response{500, "failure", {}}; });
  CHECK(Get(h.Start()) == "default"); CHECK(StillMarked(config));
  for (int mode = 0; mode != 3; ++mode) {
    h.Load(response);
    auto reset = One(Experiment("reset_" + to_string(mode)));
    if (mode == 1) reset["track_config"] = json::object();
    if (mode == 2) reset["track_config"] = {{"trigger_switch", "bad"}};
    h.Load(reset);
    CHECK(!ifstream(config).good());
    h.Restart(); h.Cache(); CHECK(Events().size() == 1);
  }
  // 从旧结果文件迁移一次，之后三个文件各自恢复。
  result_only["track_config"] = response.at("track_config");
  Harness migrated(result_only);
  CHECK(!ReadJson(migrated.path).count("track_config"));
  CHECK(ReadJson(migrated.path + ".track_config.json").at("track_config") == response.at("track_config"));
  migrated.Restart(); migrated.Cache(); CHECK(Events().empty());
  // 身份变化只删除结果，保留独立配置和跨主体命中历史。
  h.Load(response);
  MarkFile(config); MarkFile(hits);
  h.User("user_b");
  CHECK(!ifstream(h.path).good());
  CHECK(StillMarked(config) && StillMarked(hits));
  h.Restart(); CHECK(h.Cache() == "default");
  CHECK(StillMarked(config) && StillMarked(hits));
}

void TestRequestBody() {
  Harness h;
  h.Load(One(Experiment()));
  {
    lock_guard<mutex> lock(network_mtx);
    const auto &body = requests.back();
    CHECK(body.at("login_id") == "user_a");
    CHECK(body.at("platform") == "cpp");
    CHECK(body.at("abtest_lib_version") == "1.0.0");
    CHECK(!body.count("custom_ids") && !body.count("custom_properties") && !body.count("param_name"));
  }
  ABTestPropertiesNode properties;
  properties.SetString("membership", "VIP");
  CHECK(Get(h.Start("color", "default", properties)) == "red");
  {
    lock_guard<mutex> lock(network_mtx);
    CHECK(requests.back().at("custom_properties").at("membership") == "VIP");
    CHECK(requests.back().at("param_name") == "color");
  }
}

void TestBackgroundDoesNotHit() {
  const string path = work_dir + "/background.json";
  SetReply(One(Experiment()));
  const auto start = chrono::steady_clock::now();
  {
    DefaultConsumer sdk("https://ab.example.invalid", "project", path);
  }  // 初始化请求完成，定时器可立即停止，无悬空线程。
  CHECK(chrono::steady_clock::now() - start < chrono::seconds(3));
  CHECK(Events().empty());
  CHECK(!ReadJson(path).count("hit_records"));
  CHECK(!ifstream(path + ".hit_records.json").good());
  Harness restored(ReadJson(path));
  CHECK(restored.Cache() == "red");
  CHECK(Events().size() == 1);
}
}  // namespace

namespace sensors_analytics {
Response Post(const string &url, const string &data, int timeout,
              const vector<HeaderFieldItem> &headers) {
  CHECK(url == "https://ab.example.invalid");
  CHECK(timeout >= 1);
  CHECK(find(headers.begin(), headers.end(), HeaderFieldItem("project-key", "project")) != headers.end());
  const json body = json::parse(data);
  function<Response(const json &)> callback;
  {
    lock_guard<mutex> lock(network_mtx);
    requests.push_back(body);
    callback = responder;
  }
  return callback ? callback(body) : Reply({{"status", "FAILED"}});
}
}  // namespace sensors_analytics

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  work_dir = argv[1];
  event_file = work_dir + "/analytics_events.jsonl";
  CHECK(Analytics::Init(event_file, "https://analytics.example.invalid", "user_a", true, 10000));
  Analytics::AppendRecordsToDisk(true);
  const vector<pair<string, function<void()>>> cases = {
      {"新版 SaaS 示例响应、四类参数及事件字段", TestFixture},
      {"A→B→A 与同组版本变化", TestTransitions},
      {"最小出组、重复出组和重新进组", TestOutAndReentry},
      {"多个出组及同名参数事件顺序", TestOutOrdering},
      {"主体三字段隔离与请求用户变化", TestSubjects},
      {"命中记录写盘、重启恢复、刷新保留历史", TestPersistence},
      {"独立文件和写入时机", TestSeparateFiles},
      {"独立事件配置更新、恢复与删除", TestTrackConfigStorage},
      {"两份缓存独立恢复与旧合并文件迁移", TestSeparateRecovery},
      {"事件关闭仍写历史、重启开启不补发", TestSwitchAndRestart},
      {"默认配置与远程整体覆盖", TestConfigurations},
      {"配置化扩展属性及非法值过滤", TestExtensions},
      {"非法主体、非法 ID 与大整数 ID", TestInvalidRecords},
      {"旧 SaaS 回退及结果 ID 补齐", TestLegacy},
      {"请求失败保留缓存、成功空结果清理", TestFailuresAndEmpty},
      {"参数转换异常与类型不匹配时出组", TestInvalidValuesAndOut},
      {"Fast/Async 策略及回调重入、异常", TestFetchModesAndCallback},
      {"并发查询只记录并上报一次", TestConcurrentHits},
      {"同身份响应按完成顺序处理", TestResponseOrdering},
      {"身份代次拒绝 A→B→A 的旧响应", TestIdentityGeneration},
      {"旧分流文件、损坏历史及写入失败", TestCacheRecoveryAndWriteFailure},
      {"请求 body 与已有调用级属性兼容", TestRequestBody},
      {"后台拉取不命中及定时器停止", TestBackgroundDoesNotHit}};
  int failures = 0;
  for (const auto &test : cases) {
    Analytics::Identify("user_a", true);
    Analytics::ClearQueue();
    SetReply(ResponseData());
    try {
      test.second();
      cout << "[通过] " << test.first << endl;
    } catch (const exception &error) {
      ++failures;
      cerr << "[失败] " << test.first << ": " << error.what() << endl;
    }
  }
  Analytics::Shutdown();
  cout << cases.size() - failures << "/" << cases.size() << " 项通过" << endl;
  return failures ? 1 : 0;
}
