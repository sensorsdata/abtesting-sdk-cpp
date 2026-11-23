/*
 * Copyright 2015－2021 Sensors Data Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sensors_abtesting_sdk.h"

#include <algorithm>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <stdexcept>
#include <utility>

#include "sensors_json.hpp"
#include "sensors_network.h"
#include "sensors_utils.h"

namespace sensors_abtesting {

using namespace sensors_analytics;
using namespace std;
using json = sensors_json::json;

namespace abtesting_utils {

const string kCurrentUserIdKey = "user_id";
const int kDefaultTimeoutMilliseconds = 30 * 1000;
enum FetchMode { kFetchModeCache, kFetchModeAsync, kFetchModeFast };

struct UserContext {
  string distinct_id;
  bool is_login_id;
  unsigned long long generation;
};

struct ExperimentItemVar {
  string name;
  string type;
  string value;
};

struct ExperimentItem {
  string experiment_id;
  string experiment_group_id;
  string experiment_result_id;
  string subject_name;
  string subject_id;
  bool is_white_list = false;
  vector<ExperimentItemVar> variables;
  json extended_info;
};

struct HitRecord {
  string experiment_id;
  string experiment_group_id;
  string experiment_result_id;
};

// legacy 与服务端主体分开，避免字符串拼接碰撞及登录/匿名身份混用。
using SubjectKey = tuple<bool, string, string, bool>;
using HitRecords = map<SubjectKey, map<string, HitRecord>>;

struct TrackConfig {
  bool trigger_switch = true;
  vector<string> extended_keys = {
      "abtest_experiment_version", "abtest_experiment_result_id"};
};

// ID 只接受字符串或整数，不经浮点数转换，避免大 ID 丢失精度。
string ReadId(const json &object, const string &key, bool required = true) {
  const auto found = object.find(key);
  if (found == object.end() && !required) {
    return "";
  }
  if (found == object.end()) {
    throw invalid_argument("missing ID: " + key);
  }
  string value;
  if (found->is_string()) {
    value = found->get<string>();
  } else if (found->is_number_integer()) {
    value = found->dump();
  } else {
    throw invalid_argument("invalid ID type: " + key);
  }
  if (value.empty()) {
    throw invalid_argument("empty ID: " + key);
  }
  return value;
}

SubjectKey HitSubject(const ExperimentItem &item, const UserContext &context) {
  if (item.subject_name.empty()) {
    return make_tuple(true, "", context.distinct_id, context.is_login_id);
  }
  return make_tuple(false, item.subject_name, item.subject_id, false);
}

// 添加 http header 内容
vector<HeaderFieldItem> RequestHeaderFields(const string &project_key) {
  vector<HeaderFieldItem> headers;
  HeaderFieldItem user_agent("Content-Type", "application/json");
  headers.push_back(user_agent);
  HeaderFieldItem p_key("project-key", project_key);
  headers.push_back(p_key);
  return headers;
}

// default fetch completion function
void FetchCompletionFunc(const json &) {
  // nothing to do
}

#if defined(__linux__)
#define SA_SDK_LOCALTIME(seconds, now) localtime_r((seconds), (now))
#elif defined(__APPLE__)
#define SA_SDK_LOCALTIME(seconds, now) localtime_r((seconds), (now))
#elif defined(_WIN32)
#define SA_SDK_LOCALTIME(seconds, now) localtime_s((now), (seconds))
#define snprintf sprintf_s
#endif

string CurrentTime() {
  int stampTime = (int)time(NULL);
  time_t tick = (time_t)stampTime;
  struct tm tm;
  char s[100];
  SA_SDK_LOCALTIME(&tick, &tm);
  strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &tm);
  return s;
}

string LogMessagePrefix() {
  return "\n[" + CurrentTime() + "] [Sensors A/B Testing CPP SDK]: ";
}

template <typename T>
void LogInfo(T const message) {
  if (!sensors_analytics::Sdk::IsEnableLog()) {
    return;
  }
  cout << LogMessagePrefix() << message << endl;
}

template <typename T>
void LogError(T const message) {
  if (!sensors_analytics::Sdk::IsEnableLog()) {
    return;
  }
  cerr << LogMessagePrefix() << message << endl;
}

// 客户回调独立执行；回调抛异常不能被误判为解析失败并再次回调。
void CompleteFetch(FetchHandler handler, const json &result) {
  if (!handler) {
    return;
  }
  try {
    handler(result);
  } catch (const exception &err) {
    LogError("fetch handler failed: " + string(err.what()));
  } catch (...) {
    LogError("fetch handler failed with an unknown exception");
  }
}

};  // namespace abtesting_utils

using namespace abtesting_utils;

class Timer {
 public:
  void start(int minute, function<void()> task) {
    worker_ = thread([this, minute, task]() {
      unique_lock<mutex> lock(mtx_);
      while (!cv_.wait_for(lock, chrono::minutes(minute),
                           [this]() { return stopped_; })) {
        lock.unlock();
        task();
        lock.lock();
      }
    });
  }

  void stop() {
    {
      lock_guard<mutex> lock(mtx_);
      stopped_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  ~Timer() { stop(); }

 private:
  bool stopped_ = false;
  condition_variable cv_;
  mutex mtx_;
  thread worker_;
};

class HttpRequest {
 public:
  explicit HttpRequest(
      const string &server_url,
      const vector<HeaderFieldItem> &http_headers = vector<HeaderFieldItem>());

  bool Request(const string &data, const vector<HeaderFieldItem> &http_headers,
               string &response_body, int timeout_second = 30);
  string server_url_;

 private:
  vector<pair<string, string>> http_headers_;
};

HttpRequest::HttpRequest(const string &server_url,
                         const vector<HeaderFieldItem> &http_headers)
    : server_url_(server_url), http_headers_(http_headers) {}

bool HttpRequest::Request(const string &data,
                          const vector<HeaderFieldItem> &http_headers,
                          string &response_body, int timeout_second) {
  // 超时时间最小为 1s
  int timeout = timeout_second < 1 ? 1 : timeout_second;
  Response response = Post(server_url_, data, timeout, http_headers);
  if (response.code_ != 200) {
    LogError("request experiments failed:" + response.body_);
    return false;
  }
  if (response.body_.length() < 1) {
    return false;
  }
  response_body = response.body_;
  return true;
}

void ABTestPropertiesNode::SetList(const string &property_name, const std::vector<string> &value) {
    try {
        json value_json(value);
        if (!value_json.is_null()) {
            string value_string = value_json.dump();
            SetString(property_name, value_string);
        }
    } catch (exception &err) {
        LogError("ABTestPropertiesNode SetList failed to convert the value to a string : " + (string)err.what());
    }
}

void ABTestPropertiesNode::SetObject(const string &, const ObjectNode &) {}

class ABTestObserver;
class DefaultConsumer {
 public:
  ABTestObserver *observer_;

  // 内部开关用于本地确定性测试；客户 Init 仍启动初始化拉取和定时刷新。
  DefaultConsumer(const string &server_url, const string &project_key,
                  const string &experiment_file_path, bool start_background = true);
  ~DefaultConsumer();

  json FetchExperimentResult(FetchMode fetch_mode, const string &param_name,
                             const json &default_value,
                             const ABTestPropertiesNode &properties,
                             FetchHandler handler, int timeout_millisecond);

 private:
  bool Request(const string &param_name, const json &default_value,
               int timeout_millisecond, FetchHandler handler,
               const string &custom_properties);
  bool AsyncGetDefaultResult();
  void RestoreFromDisk();
  void RestoreHitRecords(const json &records);
  // 调用方持有 handle_response_mtx_，复制最新状态后写盘。
  void PersistExperiments();
  bool PersistHitRecords();
  bool PersistTrackConfig();
  bool UpdateExperiments(const json &response, const UserContext &context,
                         bool update_config = true);
  bool ParseExperiment(const json &value, bool is_out, ExperimentItem *item);
  TrackConfig ParseTrackConfig(const json &response, bool *is_remote = NULL);

  // 命中 mutex 覆盖查询、最后记录更新和 Track，handler 在所有锁外。
  json QueryAndTrack(const string &param_name, const json &default_value,
                     const UserContext *request_context = NULL);
  void TrackTriggerEvent(const ExperimentItem &item, const UserContext &context,
                         const TrackConfig &config);
  json ConvertResult(const ExperimentItem &item, const string &param_name,
                     const json &default_value);
  void AddExtendedProperties(const ExperimentItem &item, const TrackConfig &config,
                             PropertiesNode *properties);
  void HandleCompletion(const string &param_name, const json &default_value,
                        FetchHandler handler, const UserContext &context);
  UserContext CurrentUserContext();
  void StoreAsyncFuture(future<bool> task);
  bool IsSameType(const string &type, const json &default_value);
  bool AssertProperties(const utils::ObjectNode &properties);
  bool AssertKey(const string &type, const string &key);

  HttpRequest *request_;
  Timer *timer_;
  string file_path_;
  string hit_file_path_;
  string config_file_path_;
  map<string, ExperimentItem> experiments_;
  map<string, vector<ExperimentItem>> out_experiments_;
  json cached_response_;
  TrackConfig track_config_;
  json cached_track_config_;
  HitRecords hit_records_;
  mutex experiments_mtx_;  // 保护缓存、身份、事件配置及命中记录
  string distinct_id_;
  bool is_login_id_;
  unsigned long long user_generation_;
  string project_key_;
  bool is_first_trigger_event_;
  bool start_background_;
  mutex hit_mtx_;  // 串行查询和命中事件；不持此锁调用客户 handler
  mutex handle_response_mtx_;  // 串行响应提交和文件写入
  vector<future<bool>> async_futures_;
  mutex async_futures_mtx_;
};

typedef std::function<void()> ObserverCompletion;

class ABTestObserver final : public UserAlterationObserver {
 public:
  ObserverCompletion completion_;
  void Update() { completion_(); }
};

DefaultConsumer::DefaultConsumer(const string &server_url,
                                 const string &project_key,
                                 const string &experiment_file_path,
                                 bool start_background)
    : observer_(new ABTestObserver()),
      request_(new HttpRequest(server_url)),
      timer_(new Timer()),
      file_path_(experiment_file_path),
      hit_file_path_(experiment_file_path + ".hit_records.json"),
      config_file_path_(experiment_file_path + ".track_config.json"),
      distinct_id_(sensors_analytics::Sdk::DistinctID()),
      is_login_id_(sensors_analytics::Sdk::IsLoginID()),
      user_generation_(0),
      project_key_(project_key),
      is_first_trigger_event_(true),
      start_background_(start_background) {
  observer_->completion_ = [this]() {
    const string distinct_id = sensors_analytics::Sdk::DistinctID();
    const bool is_login_id = sensors_analytics::Sdk::IsLoginID();
    {
      lock_guard<mutex> response_lock(handle_response_mtx_);
      {
        lock_guard<mutex> state_lock(experiments_mtx_);
        distinct_id_ = distinct_id;
        is_login_id_ = is_login_id;
        ++user_generation_;
        experiments_.clear();
        out_experiments_.clear();
        cached_response_ = json();
      }
      // 与 iOS 一致：用户切换仅删除分流缓存，保留命中历史和事件配置。
      if (std::remove(file_path_.c_str()) != 0 && errno != ENOENT) {
        LogError("cannot remove experiment cache: " + file_path_);
      }
    }
    if (start_background_) {
      StoreAsyncFuture(async(launch::async, &DefaultConsumer::Request, this,
                             "", json(), kDefaultTimeoutMilliseconds,
                             FetchCompletionFunc, ""));
    }
  };

  RestoreFromDisk();
  if (start_background_) {
    timer_->start(10, [this]() {
      StoreAsyncFuture(async(launch::async, &DefaultConsumer::Request, this,
                             "", json(), kDefaultTimeoutMilliseconds,
                             FetchCompletionFunc, ""));
    });
    StoreAsyncFuture(async(launch::async, &DefaultConsumer::AsyncGetDefaultResult,
                           this));
  }
}

DefaultConsumer::~DefaultConsumer() {
  timer_->stop();
  vector<future<bool>> tasks;
  {
    lock_guard<mutex> lock(async_futures_mtx_);
    tasks.swap(async_futures_);
  }
  for (auto &task : tasks) {
    task.wait();
  }
  delete timer_;
  delete request_;
  delete observer_;
}

UserContext DefaultConsumer::CurrentUserContext() {
  lock_guard<mutex> lock(experiments_mtx_);
  return UserContext{distinct_id_, is_login_id_, user_generation_};
}

void DefaultConsumer::StoreAsyncFuture(future<bool> task) {
  lock_guard<mutex> lock(async_futures_mtx_);
  async_futures_.erase(remove_if(async_futures_.begin(), async_futures_.end(),
      [](future<bool> &value) {
        return value.wait_for(chrono::seconds(0)) == future_status::ready;
      }), async_futures_.end());
  async_futures_.push_back(std::move(task));
}

bool DefaultConsumer::AsyncGetDefaultResult() {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (Request("", json(), kDefaultTimeoutMilliseconds, FetchCompletionFunc, "")) {
      return true;
    }
    if (attempt < 2) {
      this_thread::sleep_for(chrono::seconds(30));
    }
  }
  return false;
}

bool DefaultConsumer::ParseExperiment(const json &value, bool is_out,
                                      ExperimentItem *item) {
  if (!value.is_object()) {
    throw invalid_argument("experiment must be an object");
  }
  if (value.count("experiment_type") && value.at("experiment_type") != "CODE") {
    return false;  // Native SDK 不索引可视化等非 CODE 试验。
  }
  const bool has_name = value.count("subject_name") != 0;
  const bool has_id = value.count("subject_id") != 0;
  if (has_name || has_id) {
    if (!has_name || !has_id || !value.at("subject_name").is_string() ||
        !value.at("subject_id").is_string() ||
        value.at("subject_name").get<string>().empty() ||
        value.at("subject_id").get<string>().empty()) {
      LogError("skip experiment with incomplete subject fields");
      return false;
    }
    item->subject_name = value.at("subject_name").get<string>();
    item->subject_id = value.at("subject_id").get<string>();
  }
  item->experiment_id = ReadId(value, "abtest_experiment_id");
  item->experiment_group_id = ReadId(value, "abtest_experiment_group_id", !is_out);
  item->experiment_result_id = ReadId(value, "abtest_experiment_result_id", is_out);
  if ((is_out && item->experiment_result_id != "-1") ||
      (!is_out && item->experiment_result_id == "-1")) {
    throw invalid_argument("invalid experiment result ID for results/out_list");
  }
  item->is_white_list = value.value("is_white_list", false);
  item->extended_info = value;
  item->extended_info["abtest_experiment_id"] = item->experiment_id;
  if (!item->experiment_group_id.empty()) {
    item->extended_info["abtest_experiment_group_id"] = item->experiment_group_id;
  }
  if (!item->experiment_result_id.empty()) {
    item->extended_info["abtest_experiment_result_id"] = item->experiment_result_id;
  }
  const json &variables = value.at("variables");
  if (!variables.is_array()) {
    throw invalid_argument("variables must be an array");
  }
  for (const auto &variable : variables) {
    ExperimentItemVar var;
    var.name = variable.at("name").get<string>();
    if (var.name.empty()) {
      continue;
    }
    if (!is_out) {
      var.type = variable.at("type").get<string>();
      var.value = variable.at("value").get<string>();
    }
    item->variables.push_back(var);
  }
  return true;
}

TrackConfig DefaultConsumer::ParseTrackConfig(const json &response, bool *is_remote) {
  if (is_remote) *is_remote = false;
  TrackConfig config;
  const auto found = response.find("track_config");
  if (found == response.end() || (found->is_object() && found->empty())) {
    return config;
  }
  try {
    if (!found->is_object()) {
      throw invalid_argument("track_config must be an object");
    }
    config.trigger_switch = found->value("trigger_switch", false);
    config.extended_keys.clear();
    if (found->count("trigger_content_ext")) {
      const json &keys = found->at("trigger_content_ext");
      if (!keys.is_array()) {
        throw invalid_argument("trigger_content_ext must be an array");
      }
      for (const auto &key : keys) {
        config.extended_keys.push_back(key.get<string>());
      }
    }
    if (found->value("property_set_switch", false)) {
      LogInfo("property_set_switch is not supported; using trigger events only");
    }
    if (is_remote) *is_remote = true;
    return config;
  } catch (const exception &err) {
    LogError("invalid track_config, using defaults: " + string(err.what()));
    return TrackConfig();
  }
}

bool DefaultConsumer::UpdateExperiments(const json &response,
                                        const UserContext &context,
                                        bool update_config) {
  try {
    if (!response.is_object() || response.at("status") != "SUCCESS" ||
        !response.at("results").is_array()) {
      return false;
    }
    map<string, ExperimentItem> experiments;
    map<string, vector<ExperimentItem>> out_experiments;
    set<string> active_experiments;
    for (const auto &value : response.at("results")) {
      ExperimentItem item;
      if (!ParseExperiment(value, false, &item)) {
        continue;
      }
      active_experiments.insert(item.experiment_id);
      for (const auto &var : item.variables) {
        experiments.insert(make_pair(var.name, item));
      }
    }
    if (response.count("out_list")) {
      if (!response.at("out_list").is_array()) {
        throw invalid_argument("out_list must be an array");
      }
      for (const auto &value : response.at("out_list")) {
        ExperimentItem item;
        if (!ParseExperiment(value, true, &item)) {
          continue;
        }
        if (active_experiments.count(item.experiment_id)) {
          LogError("skip out_list entry conflicting with results");
          continue;
        }
        for (const auto &var : item.variables) {
          out_experiments[var.name].push_back(item);
        }
      }
    }
    bool is_remote = false;
    TrackConfig config;
    if (update_config) config = ParseTrackConfig(response, &is_remote);
    lock_guard<mutex> lock(experiments_mtx_);
    if (context.generation != user_generation_) {
      return false;
    }
    experiments_.swap(experiments);
    out_experiments_.swap(out_experiments);
    if (update_config) {
      track_config_ = config;
      cached_track_config_ = is_remote ? json{{"track_config", response.at("track_config")}} : json();
    }
    cached_response_ = response;
    cached_response_.erase("hit_records");  // 后端不能覆盖本地命中历史。
    cached_response_.erase("track_config");  // 事件配置单独存储和恢复。
    return true;
  } catch (const exception &err) {
    LogError("invalid experiment response: " + string(err.what()));
    return false;
  }
}

bool DefaultConsumer::Request(const string &param_name,
                              const json &default_value,
                              int timeout_millisecond,
                              FetchHandler handler,
                              const string &custom_properties) {
  const UserContext context = CurrentUserContext();
  bool success = false;
  try {
    if (!request_->server_url_.empty()) {
      json request_body;
      request_body[context.is_login_id ? "login_id" : "anonymous_id"] =
          context.distinct_id;
      request_body["abtest_lib_version"] = ABTESTING_SDK_VERSION;
      request_body["platform"] = "cpp";
      if (custom_properties.length() > 2 && !param_name.empty()) {
        json properties = json::parse(custom_properties);
        if (!properties.is_null()) {
          request_body["custom_properties"] = properties;
          request_body["param_name"] = param_name;
        }
      }
      string response_body;  // 响应只属于本次请求，不通过共享缓冲传递。
      const vector<HeaderFieldItem> headers = RequestHeaderFields(project_key_);
      if (request_->Request(request_body.dump(), headers, response_body,
                            timeout_millisecond / 1000)) {
        json response = json::parse(response_body);
        if (response.is_object() && response.value("status", string()) == "SUCCESS") {
          lock_guard<mutex> lock(handle_response_mtx_);
          const UserContext current = CurrentUserContext();
          if (context.generation == current.generation) {
            response[kCurrentUserIdKey] = context.distinct_id;
            response["is_login_id"] = context.is_login_id;
            if (UpdateExperiments(response, context)) {
              PersistExperiments();
              PersistTrackConfig();
              success = true;
            }
          }
        } else {
          LogError("request experiments failed: " + response_body);
        }
      }
    }
  } catch (const exception &err) {
    LogError("Exception when Request: " + string(err.what()));
  }
  // 客户回调与 Track 不在响应锁内；失败、解析异常、身份过期均正常完成。
  if (!param_name.empty()) {
    if (success) {
      HandleCompletion(param_name, default_value, handler, context);
    } else {
      CompleteFetch(handler, default_value);
    }
  }
  return success;
}

void DefaultConsumer::RestoreHitRecords(const json &records) {
  if (!records.is_array()) {
    LogError("ignore invalid hit_records");
    return;
  }
  for (const auto &group : records) {
    try {
      SubjectKey key;
      if (group.count("legacy_user")) {
        const auto &user = group.at("legacy_user");
        if (group.count("subject_name") || group.count("subject_id")) {
          throw invalid_argument("mixed legacy and subject hit group");
        }
        const string id = user.at("distinct_id").get<string>();
        if (id.empty()) {
          throw invalid_argument("empty legacy distinct_id");
        }
        key = make_tuple(true, "", id, user.at("is_login_id").get<bool>());
      } else {
        const string name = group.at("subject_name").get<string>();
        const string id = group.at("subject_id").get<string>();
        if (name.empty() || id.empty()) {
          throw invalid_argument("empty hit subject");
        }
        key = make_tuple(false, name, id, false);
      }
      const json &results = group.at("results");
      if (!results.is_object()) {
        throw invalid_argument("hit results must be an object");
      }
      for (auto it = results.begin(); it != results.end(); ++it) {
        try {
          HitRecord record;
          record.experiment_id = ReadId(it.value(), "abtest_experiment_id");
          record.experiment_group_id = ReadId(it.value(), "abtest_experiment_group_id", false);
          record.experiment_result_id = ReadId(it.value(), "abtest_experiment_result_id", false);
          if (record.experiment_id != it.key() ||
              (record.experiment_group_id.empty() && record.experiment_result_id.empty())) {
            throw invalid_argument("invalid hit record");
          }
          hit_records_[key][record.experiment_id] = record;
        } catch (const exception &err) {
          LogError("ignore invalid hit record: " + string(err.what()));
        }
      }
    } catch (const exception &err) {
      LogError("ignore invalid hit group: " + string(err.what()));
    }
  }
}

void DefaultConsumer::RestoreFromDisk() {
  // 三类缓存独立恢复；结果文件损坏不影响历史和事件配置。
  ifstream config_file(config_file_path_.c_str());
  const bool has_config_file = config_file.is_open();
  if (has_config_file) {
    try {
      const string data((istreambuf_iterator<char>(config_file)), istreambuf_iterator<char>());
      bool is_remote = false;
      const json saved = json::parse(data);
      track_config_ = ParseTrackConfig(saved, &is_remote);
      if (is_remote) cached_track_config_ = saved;
    } catch (const exception &err) {
      LogError("cannot restore track config: " + string(err.what()));
    }
    config_file.close();
  }
  ifstream hit_file(hit_file_path_.c_str());
  const bool has_hit_file = hit_file.is_open();
  if (has_hit_file) {
    try {
      const string data((istreambuf_iterator<char>(hit_file)), istreambuf_iterator<char>());
      RestoreHitRecords(json::parse(data).at("hit_records"));
    } catch (const exception &err) {
      LogError("cannot restore hit records: " + string(err.what()));
    }
  }
  ifstream file(file_path_.c_str());
  if (!file) {
    return;
  }
  const string data((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
  file.close();
  if (data.empty()) {
    return;
  }
  try {
    json response = json::parse(data);
    if (!response.is_object()) {
      return;
    }
    const bool needs_cleanup = response.count("hit_records") || response.count("track_config");
    bool migration_ok = true;
    if (!has_hit_file && response.count("hit_records") && response.at("hit_records").is_array()) {
      // 兼容开发期的合并文件，仅迁移一次；独立文件存在时始终以其为准。
      RestoreHitRecords(response.at("hit_records"));
      migration_ok = PersistHitRecords();
    }
    if (!has_config_file && response.count("track_config")) {
      bool is_remote = false;
      track_config_ = ParseTrackConfig(response, &is_remote);
      if (is_remote) cached_track_config_ = {{"track_config", response.at("track_config")}};
      migration_ok = PersistTrackConfig() && migration_ok;
    }
    cached_response_ = response;
    cached_response_.erase("hit_records");
    cached_response_.erase("track_config");
    if (needs_cleanup && migration_ok) PersistExperiments();  // 迁移成功后立即拆开旧合并文件。
    const UserContext context = CurrentUserContext();
    if (response.value(kCurrentUserIdKey, string()) != context.distinct_id ||
        (response.count("is_login_id") &&
         response.at("is_login_id").get<bool>() != context.is_login_id)) {
      return;  // 只拒绝结果缓存，保留历史和配置。
    }
    response["is_login_id"] = context.is_login_id;
    UpdateExperiments(response, context, false);
  } catch (const exception &err) {
    LogError("cannot restore experiment cache: " + string(err.what()));
  }
}

void DefaultConsumer::PersistExperiments() {
  json data;
  {
    lock_guard<mutex> lock(experiments_mtx_);
    data = cached_response_;
  }
  if (!data.is_object()) return;
  ofstream file(file_path_.c_str(), ofstream::out | ofstream::trunc);
  if (!file) {
    LogError("cannot open experiment cache for writing: " + file_path_);
    return;
  }
  file << data.dump() << endl;
  file.close();
  if (!file) LogError("cannot write experiment cache: " + file_path_);
}

bool DefaultConsumer::PersistHitRecords() {
  json data;
  {
    lock_guard<mutex> lock(experiments_mtx_);
    data = json::object();
    data["hit_records"] = json::array();
    for (const auto &group : hit_records_) {
      json entry;
      if (get<0>(group.first)) {
        entry["legacy_user"] = {{"distinct_id", get<2>(group.first)},
                                {"is_login_id", get<3>(group.first)}};
      } else {
        entry["subject_name"] = get<1>(group.first);
        entry["subject_id"] = get<2>(group.first);
      }
      entry["results"] = json::object();
      for (const auto &value : group.second) {
        const HitRecord &record = value.second;
        json result = {{"abtest_experiment_id", record.experiment_id}};
        if (!record.experiment_group_id.empty()) {
          result["abtest_experiment_group_id"] = record.experiment_group_id;
        }
        if (!record.experiment_result_id.empty()) {
          result["abtest_experiment_result_id"] = record.experiment_result_id;
        }
        entry["results"][value.first] = result;
      }
      data["hit_records"].push_back(entry);
    }
  }
  ofstream file(hit_file_path_.c_str(), ofstream::out | ofstream::trunc);
  if (!file) {
    LogError("cannot open hit records for writing: " + hit_file_path_);
    return false;
  }
  file << data.dump() << endl;
  file.close();
  if (!file) {
    LogError("cannot write hit records: " + hit_file_path_);
    return false;
  }
  return true;
}

bool DefaultConsumer::PersistTrackConfig() {
  json data;
  {
    lock_guard<mutex> lock(experiments_mtx_);
    data = cached_track_config_;
  }
  // 与 iOS 一致：没有有效远程配置时删除配置文件，恢复默认配置。
  if (data.is_null()) {
    if (std::remove(config_file_path_.c_str()) != 0 && errno != ENOENT) {
      LogError("cannot remove track config: " + config_file_path_);
      return false;
    }
    return true;
  }
  ofstream file(config_file_path_.c_str(), ofstream::out | ofstream::trunc);
  if (!file) {
    LogError("cannot open track config for writing: " + config_file_path_);
    return false;
  }
  file << data.dump() << endl;
  file.close();
  if (!file) {
    LogError("cannot write track config: " + config_file_path_);
    return false;
  }
  return true;
}

bool DefaultConsumer::AssertKey(const string &type, const string &key) {
    size_t len = key.length();
    if (len < 1 || len > 100) {
        LogError("The " + type + " is empty or too long, max length is 100");
        return false;
    }
    char ch = key[0];
    if ((ch >= 'a' && ch <= 'z') || ch == '$' ||
        (ch >= 'A' && ch <= 'Z') || ch == '_') {
        for (size_t i = 1; i < len; ++i) {
            ch = key[i];
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                (ch >= 'A' && ch <= 'Z') || ch == '$' || ch == '_') {
                continue;
            }
            LogError("The " + type + " need to be a valid variable name.");
            return false;
        }
        return true;
    } else {
        LogError("The " + type + " need to be a valid variable name.");
        return false;
    }
}

bool DefaultConsumer::AssertProperties(const utils::ObjectNode &properties) {
    for (std::map<string, utils::ObjectNode::ValueNode>::const_iterator iter = properties.properties_map_.begin(); iter != properties.properties_map_.end(); ++iter) {
        
        if (!AssertKey("Property Key", iter->first)) {
            return false;
        }
        
        if (iter->second.node_type_ != utils::ObjectNode::STRING &&
            iter->second.node_type_ != utils::ObjectNode::LIST &&
            iter->second.node_type_ != utils::ObjectNode::NUMBER &&
            iter->second.node_type_ != utils::ObjectNode::INT &&
            iter->second.node_type_ != utils::ObjectNode::BOOL &&
            iter->second.node_type_ != utils::ObjectNode::DATETIME) {
            LogError("The property values must be STRING, LIST, NUMBER, BOOL or DATETIME. The value of property [" + iter->first + "] is invalid!");
            return false;
        }
    }
    return true;
}

json DefaultConsumer::FetchExperimentResult(FetchMode fetch_mode,
                                            const string &param_name,
                                            const json &default_value,
                                            const ABTestPropertiesNode &properties,
                                            FetchHandler handler,
                                            int timeout_milliseconds) {
  if (handler == NULL) {
    LogError("fetch param_name [" + param_name + "] failed, handler must be exist!");
    return default_value;
  }

  if (param_name.length() < 1) {
    LogError("param_name: " + param_name +
             " error，param_name must be a valid string!");
    CompleteFetch(handler, default_value);
    return default_value;
  }

  if (!(default_value.is_number_integer() || default_value.is_string() ||
        default_value.is_boolean() || default_value.is_object())) {
    LogError("fetch param_name [" + param_name + "] failed, default_value type must be int/string/bool/json !");
    CompleteFetch(handler, default_value);
    return default_value;
  }

  if (!AssertProperties(properties)) {
    CompleteFetch(handler, default_value);
    return default_value;
  }

  if (fetch_mode == kFetchModeCache || fetch_mode == kFetchModeFast) {
    const json result = QueryAndTrack(param_name, default_value);
    if (fetch_mode == kFetchModeCache) {
      return result.is_null() ? default_value : result;
    }
    if (!result.is_null()) {
      CompleteFetch(handler, result);
      return result;
    }
  }
  string custom_properties;
  try {
    custom_properties = utils::ObjectNode::ToJson(properties);
  } catch (const exception &err) {
    LogError("cannot serialize request properties: " + string(err.what()));
    CompleteFetch(handler, default_value);
    return default_value;
  }
  StoreAsyncFuture(async(launch::async, &DefaultConsumer::Request,
                         this, param_name, default_value, timeout_milliseconds,
                         handler, custom_properties));
  return default_value;
}

json DefaultConsumer::ConvertResult(const ExperimentItem &item,
                                    const string &param_name,
                                    const json &default_value) {
  try {
    for (const ExperimentItemVar &var : item.variables) {
      if (var.name != param_name) {
        continue;
      }
      if (!IsSameType(var.type, default_value)) {
        LogError("experiment result type does not match default value: " + param_name);
        return json();
      }
      if (var.type == "INTEGER") {
        size_t consumed = 0;
        const int result = stoi(var.value, &consumed);
        return consumed == var.value.size() ? json(result) : json();
      }
      if (var.type == "BOOLEAN") {
        if (var.value != "true" && var.value != "false") {
          return json();
        }
        return var.value == "true";
      }
      if (var.type == "STRING") {
        return var.value;
      }
      if (var.type == "JSON") {
        const json result = json::parse(var.value);
        return result.is_object() ? result : json();
      }
    }
  } catch (const exception &err) {
    LogError("invalid experiment result: " + string(err.what()));
  }
  return json();
}

json DefaultConsumer::QueryAndTrack(const string &param_name,
                                    const json &default_value,
                                    const UserContext *request_context) {
  lock_guard<mutex> hit_lock(hit_mtx_);
  ExperimentItem item;
  bool has_result = false;
  vector<ExperimentItem> out_results;
  TrackConfig config;
  UserContext context;
  {
    lock_guard<mutex> lock(experiments_mtx_);
    context = UserContext{distinct_id_, is_login_id_, user_generation_};
    if (request_context && request_context->generation != context.generation) {
      return json();
    }
    const auto found = experiments_.find(param_name);
    if (found != experiments_.end()) {
      item = found->second;
      has_result = true;
    }
    const auto out = out_experiments_.find(param_name);
    if (out != out_experiments_.end()) {
      out_results = out->second;
    }
    config = track_config_;
  }
  // 出组不需要正常参数值或类型；即使业务拿到默认值，也应更新出组。
  for (const auto &out : out_results) {
    TrackTriggerEvent(out, context, config);
  }
  const json result = has_result ? ConvertResult(item, param_name, default_value) : json();
  if (!result.is_null()) {
    TrackTriggerEvent(item, context, config);
  }
  return CurrentUserContext().generation == context.generation ? result : json();
}

bool DefaultConsumer::IsSameType(const string &type,
                                 const json &default_value) {
  if (type == "INTEGER" && default_value.is_number_integer()) {
    return true;
  }
  if (type == "BOOLEAN" && default_value.is_boolean()) {
    return true;
  }
  if (type == "STRING" && default_value.is_string()) {
    return true;
  }
  if (type == "JSON" && default_value.is_object()) {
    return true;
  }
  return false;
}

void DefaultConsumer::HandleCompletion(const string &param_name,
                                       const json &default_value,
                                       FetchHandler handler,
                                       const UserContext &request_context) {
  const json result = QueryAndTrack(param_name, default_value, &request_context);
  CompleteFetch(handler, result.is_null() ? default_value : result);
}

void DefaultConsumer::AddExtendedProperties(const ExperimentItem &item,
                                            const TrackConfig &config,
                                            PropertiesNode *properties) {
  for (const string &key : config.extended_keys) {
    if (key.empty() || key[0] == '$' || key == "abtest_experiment_id" ||
        key == "abtest_experiment_group_id" || key == "lib_plugin_version" ||
        key == "distinct_id" || key == "login_id" || key == "anonymous_id" ||
        key.compare(0, 4, "sab_") == 0 || !AssertKey("extended property", "$" + key)) {
      continue;
    }
    const auto found = item.extended_info.find(key);
    if (found == item.extended_info.end() || found->is_null()) {
      continue;
    }
    const string name = "$" + key;
    if (found->is_string()) {
      properties->SetString(name, found->get<string>());
    } else if (found->is_boolean()) {
      properties->SetBool(name, found->get<bool>());
    } else if (found->is_number_integer()) {
      // 底层属性整数为 int64_t，不将超界无符号整数截断为负数。
      if (found->is_number_unsigned() &&
          found->get<uint64_t>() > uint64_t(numeric_limits<int64_t>::max())) {
        LogError("skip oversized integer extended property: " + key);
      } else {
        properties->SetNumber(name, found->get<int64_t>());
      }
    } else if (found->is_number_float() && isfinite(found->get<double>())) {
      properties->SetNumber(name, found->get<double>());
    } else if (found->is_array() && all_of(found->begin(), found->end(),
                                         [](const json &v) { return v.is_string(); })) {
      properties->SetList(name, found->get<vector<string>>());
    } else {
      LogError("skip unsupported extended property: " + key);
    }
  }
}

void DefaultConsumer::TrackTriggerEvent(const ExperimentItem &item,
                                       const UserContext &context,
                                       const TrackConfig &config) {
  if (item.is_white_list) {
    return;
  }
  {
    lock_guard<mutex> write_lock(handle_response_mtx_);
    {
      lock_guard<mutex> state_lock(experiments_mtx_);
      if (context.generation != user_generation_) return;
      auto &records = hit_records_[HitSubject(item, context)];
      const auto found = records.find(item.experiment_id);
      HitRecord next{item.experiment_id, item.experiment_group_id,
                     item.experiment_result_id};
      if (found != records.end()) {
        const HitRecord &previous = found->second;
        const bool changed = !previous.experiment_result_id.empty() && !next.experiment_result_id.empty()
            ? previous.experiment_result_id != next.experiment_result_id
            : previous.experiment_group_id != next.experiment_group_id;
        if (!changed) {
          // 旧协议同组补齐结果 ID 仅更新内存，重复命中不重写文件。
          if (!next.experiment_result_id.empty()) records[item.experiment_id] = next;
          return;
        }
      }
      records[item.experiment_id] = next;
    }
    PersistHitRecords();
  }
  // iOS 先更新命中历史，再按开关决定是否发送触发事件。
  if (!config.trigger_switch) return;
  try {
    PropertiesNode properties;
    properties.SetString("$abtest_experiment_id", item.experiment_id);
    if (!item.experiment_group_id.empty()) {
      properties.SetString("$abtest_experiment_group_id", item.experiment_group_id);
    }
    AddExtendedProperties(item, config, &properties);
    if (is_first_trigger_event_) {
      properties.SetList("$lib_plugin_version",
                         vector<string>(1, "cpp_abtesting:" + string(ABTESTING_SDK_VERSION)));
    }
    // 调用时仅持命中串行锁，不持状态锁或响应/写盘锁。
    sensors_analytics::Sdk::Track("$ABTestTrigger", properties);
    is_first_trigger_event_ = false;
  } catch (const exception &err) {
    LogError("track experiment failed: " + string(err.what()));
  }
}

Sdk *Sdk::instance_ = NULL;

bool Sdk::Init(const string &server_url, const string &experiment_file_path) {
  if (!instance_) {
    UrlParser *parser = UrlParser::parseUrl(server_url);
    string project_key;
    if (parser->queryItems.find("project-key") != parser->queryItems.end()) {
      project_key = parser->queryItems.find("project-key")->second;
    }

    string url_without_query = UrlWithoutQuery(parser);
    if (url_without_query.length() < 1 || project_key.length() < 1) {
      LogError("Initialize the SDK with the invalid server url !");
      return false;
    }
      
    if (experiment_file_path.length() < 1) {
        LogError("Initialize the SDK with the invalid experiment file path !");
        return false;
    }

    string distinct_id = sensors_analytics::Sdk::DistinctID();
    if (distinct_id.length() < 1) {
      LogError("You must initialize sensors analytics SDK before using A/B Testing SDK !");
      return false;
    }
    LogInfo("Initialize SDK successfully.");
    instance_ = new Sdk(url_without_query, project_key, experiment_file_path);
  }
  return true;
}

json Sdk::FetchCacheABTest(const string &param_name,
                           const json &default_value) {
  if (instance_) {
    ABTestPropertiesNode properties;
    return instance_->consumer_->FetchExperimentResult(kFetchModeCache, param_name, default_value, properties, FetchCompletionFunc, kDefaultTimeoutMilliseconds);
  }
  return default_value;
}

void Sdk::FastFetchABTest(const string &param_name, const json &default_value,
                          FetchHandler handler, int timeout_milliseconds) {
    ABTestPropertiesNode properties;
    FastFetchABTest(param_name, default_value, properties, handler, timeout_milliseconds);
}

void Sdk::AsyncFetchABTest(const string &param_name, const json &default_value,
                           FetchHandler handler, int timeout_milliseconds) {
    ABTestPropertiesNode properties;
    AsyncFetchABTest(param_name, default_value, properties, handler, timeout_milliseconds);
}

void Sdk::AsyncFetchABTest(const string &param_name,
                           const json &default_value,
                           const ABTestPropertiesNode &properties,
                           FetchHandler handler,
                           int timeout_milliseconds) {
    if (instance_) {
      instance_->consumer_->FetchExperimentResult(kFetchModeAsync, param_name, default_value, properties, handler, timeout_milliseconds);
    }
}
  
void Sdk::FastFetchABTest(const string &param_name,
                          const json &default_value,
                          const ABTestPropertiesNode &properties,
                          FetchHandler handler,
                          int timeout_milliseconds) {
    if (instance_) {
        instance_->consumer_->FetchExperimentResult(kFetchModeFast, param_name, default_value, properties, handler, timeout_milliseconds);
    }
}

Sdk::Sdk(const string &server_url, const string &project_key, const string &experiment_file_path)
    : consumer_(new DefaultConsumer(server_url, project_key, experiment_file_path)) {
  sensors_analytics::Sdk::Attach(consumer_->observer_);
}

Sdk::~Sdk() {
  if (consumer_ != NULL) {
    sensors_analytics::Sdk::Detach(consumer_->observer_);
    delete consumer_;
    consumer_ = NULL;
  }
}
}  // namespace sensors_abtesting
