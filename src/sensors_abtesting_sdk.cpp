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
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>

#include "sensors_analytics_sdk.h"
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

typedef struct {
  string name;
  string type;
  string value;

} ExperimentItemVar;

typedef struct {
  string experiment_id;
  string experiment_group_id;
  bool is_control_group;
  bool is_white_list;
  vector<ExperimentItemVar> variables;

} ExperimentItem;

// nlohmann/json.hpp 重载函数
void from_json(const json &j, ExperimentItemVar &var) {
  j.at("name").get_to(var.name);
  j.at("type").get_to(var.type);
  j.at("value").get_to(var.value);
}

// nlohmann/json.hpp 重载函数
void from_json(const json &j, ExperimentItem &item) {
  j.at("abtest_experiment_id").get_to(item.experiment_id);
  j.at("abtest_experiment_group_id").get_to(item.experiment_group_id);
  j.at("is_control_group").get_to(item.is_control_group);
  j.at("is_white_list").get_to(item.is_white_list);
  j.at("variables").get_to(item.variables);
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
void FetchCompletionFunc(const json &result) {
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

};  // namespace abtesting_utils

using namespace abtesting_utils;

class Timer {
 public:
  Timer() {
    expired = true;
    tryToExpire = false;
  }

  void start(int minute, std::function<void()> task) {
    if (expired == false) return;
    expired = false;
    std::thread([this, minute, task]() {
      while (!tryToExpire) {
        // sleep every interval and do the task again and again until times up
        std::this_thread::sleep_for(std::chrono::minutes(minute));
        task();  // call this function every interval milliseconds.
      }
      // thread
      std::lock_guard<std::mutex> locker(mut);
      expired = true;
      cv.notify_one();
    }).detach();
  }
  void stop() {
    // do not stop again
    if (expired) return;
    if (tryToExpire) return;
    tryToExpire = true;
    std::unique_lock<std::mutex> locker(mut);
    cv.wait(locker, [this] { return expired == true; });
    if (expired == true) tryToExpire = false;
  }

 private:
  condition_variable cv;
  mutex mut;
  atomic<bool> expired;      // timer stop status
  atomic<bool> tryToExpire;  // timer is in stop process.
};

class HttpRequest {
 public:
  explicit HttpRequest(
      const string &server_url,
      const vector<HeaderFieldItem> &http_headers = vector<HeaderFieldItem>());

  bool Request(const string &data, const vector<HeaderFieldItem> &http_headers,
               int timeout_second = 30);
  string server_url_;
  string response_body_;
  mutex response_body_mtx_;

 private:
  vector<pair<string, string>> http_headers_;
};

HttpRequest::HttpRequest(const string &server_url,
                         const vector<HeaderFieldItem> &http_headers)
    : server_url_(server_url), http_headers_(http_headers) {}

bool HttpRequest::Request(const string &data,
                          const vector<HeaderFieldItem> &http_headers,
                          int timeout_second) {
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
  response_body_mtx_.lock();
  response_body_ = response.body_;
  response_body_mtx_.unlock();
  return true;
}

class ABTestObserver;
class DefaultConsumer {
 public:
  ABTestObserver *observer_;

  DefaultConsumer(const string &server_url, const string &project_key);

  json FetchExperimentResult(FetchMode fetch_mode, const string &param_name,
                             const json &default_value, FetchHandler success,
                             int timeout_millisecond);

 private:
  bool Request(const string &param_name, const json &default_value,
               int timeout_millisecond, FetchHandler handler);

  // 初始化时异步获取实验结果
  bool AsyncGetDefaultResult();

  // 从本地磁盘中加载缓存实验结果
  string LoadExperimentsFromDisk();

  // 将实验结果缓存至本地磁盘中
  void DumpExperimentsToDisk(const string &original_data);

  // 更新内存中实验结果列表
  void ClearExperimentsFromDisk();

  // 更新内存中实验结果列表
  void UpdateExperiments(const string &original_data, bool is_request);

  // 触发 $ABTestTrigger 事件
  void TrackTriggerEvent(const string &param_name);

  // 查找指定实验结果
  json FindExperimentResult(const string &param_name,
                            const json &default_value);

  // 处理请求成功后结果回调
  void HandleCompletion(const string &param_name, const json &default_value,
                        FetchHandler handler);

  // 当前实验是否已触发过 $ABTestTrigger 事件
  bool IsTriggeredForExperiment(const string &experiment_id);

  // 标记实验已触发 $ABTestTrigger 事件
  void AddTriggeredExperiment(const string &experiment_id);

  // 匹配实验结果和默认值类型是否一致
  bool IsSameType(const string &type, const json &default_value);

  HttpRequest *request_;  // http request

  Timer *timer_;
  string file_path_;  // experiments cache file path
  map<string, ExperimentItem>
      experiments_;        // parse all experiments from original data
  mutex experiments_mtx_;  // experiments_ mutex lock

  string distinct_id_;  // user unique id from SA SDK
  bool is_login_id_;    // distinct_id is login_id or not
  string project_key_;  // A/B testing SDK reqeust header key

  bool is_first_trigger_event_;  // 是否为触发的第一个 $ABTestTrigger 事件
  vector<pair<string, vector<string>>>
      triggered_experiments_;  // 当前用户已触发 $ABTestTrigger 事件的实验 ID
                               // 对照表
  mutex triggered_experiments_mtx_;  // triggered_experiments_ mutex lock

  int retry_count_;  // 初始化时请求实验错误重试次数
  vector<future<bool>>
      async_futures_;  // 异步操作结果对象列表，保证异步操作不会被释放
};

typedef std::function<void()> ObserverCompletion;

class ABTestObserver : public UserAlterationObserver {
 public:
  ObserverCompletion completion_;
  void Update() { completion_(); }
};

DefaultConsumer::DefaultConsumer(const string &server_url,
                                 const string &project_key)
    : is_first_trigger_event_(true),
      project_key_(project_key),
      retry_count_(0),
      observer_(new ABTestObserver()),
      timer_(new Timer()),
      request_(new HttpRequest(server_url)) {
  // 观察者，SA SDK 的 distinct_id 发生变化时重新请求实验
  observer_->completion_ = [this]() {
    LogInfo("current user id did alternated ！");
    this->distinct_id_ = sensors_analytics::Sdk::DistinctID();
    this->is_login_id_ = sensors_analytics::Sdk::IsLoginID();
    this->async_futures_.push_back(
        async(launch::async, &DefaultConsumer::Request, this, "", json(),
              kDefaultTimeoutMilliseconds, FetchCompletionFunc));
  };

  // 定时器，每隔 10 分钟重新请求实验
  timer_->start(10, [this]() {
    this->async_futures_.push_back(
        async(launch::async, &DefaultConsumer::Request, this, "", json(),
              kDefaultTimeoutMilliseconds, FetchCompletionFunc));
  });

  vector<string> all_path =
      Split(sensors_analytics::Sdk::StagingFilePath(), "/");
  vector<string> prefix_path(all_path.begin(), all_path.end() - 1);
  file_path_ = (prefix_path.size() > 0 ? Splice(prefix_path, "/") : ".") +
               "/sensors_abtesting_experiments";

  distinct_id_ = sensors_analytics::Sdk::DistinctID();
  is_login_id_ = sensors_analytics::Sdk::IsLoginID();

  string original_data = LoadExperimentsFromDisk();
  UpdateExperiments(original_data, false);

  // 异步处理请求失败重试场景
  async_futures_.push_back(
      async(launch::async, &DefaultConsumer::AsyncGetDefaultResult, this));
}

bool DefaultConsumer::AsyncGetDefaultResult() {
  retry_count_++;
  future<bool> future =
      async(launch::async, &DefaultConsumer::Request, this, "", json(),
            kDefaultTimeoutMilliseconds, FetchCompletionFunc);
  if (!future.get() && retry_count_ < 3) {
    // 等待 30s 后进行重试操作
    std::this_thread::sleep_for(std::chrono::seconds(30));
    AsyncGetDefaultResult();
  }
  return true;
}

void DefaultConsumer::UpdateExperiments(const string &original_data, bool is_request) {
  if (original_data.length() < 1) {
    return;
  }

  json object;
  try {
    object = json::parse(original_data);
  } catch (exception &err) {
    LogError("Exception when Update Experiments : " + (string)err.what());
  }

  if (object.is_null()) {
    return;
  }

  if (object[kCurrentUserIdKey] != distinct_id_) {
    LogInfo("clear experiments from disk because of current user changed!");
    ClearExperimentsFromDisk();
    return;
  }

  vector<ExperimentItem> results =
      object["results"].get<vector<ExperimentItem>>();

  map<string, ExperimentItem> experiments;
  for (vector<ExperimentItem>::const_iterator item = results.begin();
       item != results.end(); ++item) {
    vector<ExperimentItemVar> variables = item->variables;
    for (vector<ExperimentItemVar>::const_iterator var = variables.begin();
         var != variables.end(); ++var) {
      // 插入内容时若对应字段名已存在，不会添加覆盖。
      experiments.insert(pair<string, ExperimentItem>(var->name, *item));
    }
  }

  if (experiments.size() < 1) {
    return;
  }

  string prefix = is_request ? "request experiments success!\n" : "load experiments success from disk!\n";
  LogInfo(prefix + original_data);

  // 互斥锁保证多线程读写安全
  experiments_mtx_.lock();
  experiments_ = experiments;
  experiments_mtx_.unlock();
}

bool DefaultConsumer::Request(const string &param_name,
                              const json &default_value,
                              int timeout_millisecond, FetchHandler handler) {
  if (request_->server_url_.length() < 1) {
    return false;
  }

  // 拷贝当前请求时的 distinct_id 内容，请求完成后对比是否为相同用户
  string temp_distinct_id(distinct_id_);

  json request_body;
  request_body[(is_login_id_ ? "login_id" : "anonymous_id")] = distinct_id_;
  request_body["abtest_lib_version"] = ABTESTING_SDK_VERSION;
  request_body["platform"] = "cpp";
  string body = request_body.dump();

  vector<HeaderFieldItem> headers = RequestHeaderFields(project_key_);
  // 转换毫秒数为秒数
  int timeout_second = timeout_millisecond / 1000;
  try {
    if (!request_->Request(body, headers, timeout_second)) {
      // 接口请求失败时，直接返回默认值
      handler(default_value);
      return false;
    }
    string response_body;
    request_->response_body_mtx_.lock();
    response_body = request_->response_body_;
    request_->response_body_mtx_.unlock();

    json response = json::parse(response_body);
    if (response["status"] != "SUCCESS") {
      // 当请求返回失败时， param_name 修改为空字符串，模拟类型匹配失败
      LogError("request experiments failed!!!" + response_body);
      HandleCompletion(param_name, default_value, handler);
      return false;
    }
    // 当接口请求时的 distinct_id 和当前 distinct_id 不一致时，需要重新请求
    if (temp_distinct_id != distinct_id_) {
      return Request(param_name, default_value, timeout_millisecond, handler);
    }

    response[kCurrentUserIdKey] = distinct_id_;
    string original_data = response.dump();
    DumpExperimentsToDisk(original_data);
    UpdateExperiments(original_data, true);

    // callback when request success
    HandleCompletion(param_name, default_value, handler);
  } catch (exception &err) {
    LogError("Exception when Request : " + (string)err.what());
  }
  return true;
}

inline void DefaultConsumer::ClearExperimentsFromDisk() {
  ofstream staging_ofs(file_path_.c_str(), ofstream::out | ofstream::trunc);
  staging_ofs.close();
}

inline string DefaultConsumer::LoadExperimentsFromDisk() {
  string original_data;
  ifstream staging_ifs(file_path_.c_str(), ofstream::in);
  staging_ifs >> original_data;
  staging_ifs.close();
  return original_data;
}

inline void DefaultConsumer::DumpExperimentsToDisk(
    const string &original_data) {
  ofstream staging_ofs(file_path_.c_str(), ofstream::out | ofstream::trunc);
  staging_ofs << original_data << endl;
  staging_ofs.close();
}

json DefaultConsumer::FetchExperimentResult(FetchMode fetch_mode,
                                            const string &param_name,
                                            const json &default_value,
                                            FetchHandler handler,
                                            int timeout_milliseconds) {
  if (param_name.length() < 1) {
    LogError("param_name: " + param_name +
             " error，param_name must be a valid string!");
    return default_value;
  }

  if (!(default_value.is_number_integer() || default_value.is_string() ||
        default_value.is_boolean() || default_value.is_object())) {
    LogError("fetch param_name [" + param_name + "] failed, default_value type must be int/string/bool/json !");
    return default_value;
  }

  if (handler == NULL) {
    LogError("fetch param_name [" + param_name + "] failed, handler must be exist!");
    return default_value;
  }

  if (fetch_mode == kFetchModeCache) {
    json result = FindExperimentResult(param_name, default_value);
    if (result.is_null()) {
      return default_value;
    }
    TrackTriggerEvent(param_name);
    return result;
  } else if (fetch_mode == kFetchModeFast) {
    json result = FindExperimentResult(param_name, default_value);
    if (!result.is_null()) {
      // 当本地缓存数据不为空时，触发事件并直接返回实验结果
      TrackTriggerEvent(param_name);
      handler(result);
      return result;
    }
    // 读取 Cache 失败后，尝试请求网络
    async_futures_.push_back(async(launch::async, &DefaultConsumer::Request,
                                   this, param_name, default_value,
                                   timeout_milliseconds, handler));
  } else if (fetch_mode == kFetchModeAsync) {
    // 直接通过请求网络获取实验数据
    async_futures_.push_back(async(launch::async, &DefaultConsumer::Request,
                                   this, param_name, default_value,
                                   timeout_milliseconds, handler));
  }
  return default_value;
}

json DefaultConsumer::FindExperimentResult(const string &param_name,
                                           const json &default_value) {
  // 初始化空返回值
  json result;

  // 当查询的计划参数名无效时，直接返回
  if (param_name.length() < 1) {
    LogError("The cache experiment result is empty, param_name:" +
               param_name);
    return result;
  }

  // 使用临时变量，减少对成员变量的读取操作
  map<string, ExperimentItem> experiments;
  experiments_mtx_.lock();
  experiments = experiments_;
  experiments_mtx_.unlock();

  // 当前计划列表为空时，直接返回
  if (experiments.size() < 1) {
    LogError("The cache experiment result is empty, param_name:" +
                 param_name);
    return result;
  }

  // 未查找到对应内容时，直接返回
  if (experiments.find(param_name) == experiments.end()) {
    LogError("The cache experiment result is empty, param_name:" +
                   param_name);
    return result;
  }

  ExperimentItem item = experiments.find(param_name)->second;
  vector<ExperimentItemVar> variables = item.variables;
  vector<ExperimentItemVar>::iterator iter;
  ExperimentItemVar var;
  for (iter = variables.begin(); iter != variables.end(); ++iter) {
    if (iter->name == param_name) {
      var = *iter;
    }
  }
  if (!IsSameType(var.type, default_value)) {
    string err_msg = "param_name [" + param_name + "] :" + "result value [" +
                     var.value + "] of type [" + var.type +
                     "] is not same for default value [" +
                     default_value.dump() + "] !";
    LogError(err_msg);
    return result;
  }

  // 类型转换，根据实验结果类型将 value 转换成对应类型
  if (var.type == "INTEGER") {
    result = stoi(var.value);
  } else if (var.type == "BOOLEAN") {
    result = (var.value == "true");
  } else if (var.type == "STRING") {
    result = var.value;
  } else if (var.type == "JSON") {
    result = json::parse(var.value);
  }
  return result;
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
                                       FetchHandler handler) {

  if (param_name.length() < 1) {
    // 当查询计划为空时，表示为内部自动请求回调，直接 return 即可
    return;
  }

  json result = FindExperimentResult(param_name, default_value);
  if (result.is_null()) {
    // 获取实验结果失败，返回默认值
    handler(default_value);
    return;
  }
  handler(result);
  TrackTriggerEvent(param_name);
}

bool DefaultConsumer::IsTriggeredForExperiment(const string &experiment_id) {
  typedef pair<string, vector<string>> TriggerdItem;
  vector<TriggerdItem> triggered;
  triggered_experiments_mtx_.lock();
  triggered = triggered_experiments_;
  triggered_experiments_mtx_.unlock();

  for (vector<TriggerdItem>::iterator iter = triggered.begin();
       iter != triggered.end(); ++iter) {
    if (iter->first == distinct_id_) {
      vector<string> list = iter->second;
      return find(list.begin(), list.end(), experiment_id) != list.end();
    }
  }
  return false;
}

void DefaultConsumer::AddTriggeredExperiment(const string &experiment_id) {
  typedef pair<string, vector<string>> TriggerdItem;

  vector<TriggerdItem> triggered;
  triggered_experiments_mtx_.lock();
  triggered = triggered_experiments_;
  triggered_experiments_mtx_.unlock();

  bool add_success = false;
  for (vector<TriggerdItem>::iterator iter = triggered.begin();
       iter != triggered.end(); ++iter) {
    if (iter->first == distinct_id_) {
      iter->second.push_back(experiment_id);
      add_success = true;
    }
  }
  // 当前 triggered_experiments_ 中无对应 distinct_id 时需要新增
  if (!add_success) {
    vector<string> experiments_ids;
    experiments_ids.push_back(experiment_id);
    triggered.push_back(TriggerdItem(distinct_id_, experiments_ids));
  }

  triggered_experiments_mtx_.lock();
  triggered_experiments_ = triggered;
  triggered_experiments_mtx_.unlock();
}

void DefaultConsumer::TrackTriggerEvent(const string &param_name) {
  map<string, ExperimentItem> experiments;
  triggered_experiments_mtx_.lock();
  experiments = experiments_;
  triggered_experiments_mtx_.unlock();

  if (experiments.size() < 1) {
    return;
  }

  ExperimentItem item = experiments.find(param_name)->second;
  bool is_contained = IsTriggeredForExperiment(item.experiment_id);
  if (is_contained || item.is_white_list) {
    return;
  }

  // 触发 $ABTestTrigger 事件
  PropertiesNode properties;
  properties.SetString("$abtest_experiment_id", item.experiment_id);
  properties.SetString("$abtest_experiment_group_id", item.experiment_group_id);

  // 当触发过 trigger event 后修改标志位为 false
  if (is_first_trigger_event_) {
    is_first_trigger_event_ = false;
    vector<string> version;
    string sdk_version = ABTESTING_SDK_VERSION;
    version.push_back("cpp_abtesting:" + sdk_version);
    properties.SetList("$lib_plugin_version", version);
  }
  sensors_analytics::Sdk::Track("$ABTestTrigger", properties);

  // 记录当前用户已触发过此实验的 $ABTestTrigger 事件
  AddTriggeredExperiment(item.experiment_id);
}

Sdk *Sdk::instance_ = NULL;

/// class Sdk
#define RETURN_IF_ERROR(stmt) \
  do {                        \
    if (!stmt) return false;  \
  } while (false)

bool Sdk::Init(const string &server_url) {
  if (!instance_) {
    UrlParser *parser = UrlParser::parseUrl(server_url);
    string project_key;
    if (parser->queryItems.find("project-key") != parser->queryItems.end()) {
      project_key = parser->queryItems.find("project-key")->second;
    }

    string url_without_query = UrlWithoutQuery(parser);
    if (url_without_query.length() < 1 || project_key.length() < 1) {
      LogError("Initialize the SDK with the valid URL");
      RETURN_IF_ERROR(true);
      return false;
    }

    string distinct_id = sensors_analytics::Sdk::DistinctID();
    if (distinct_id.length() < 1) {
      LogError(
          "You must initialize sensors analytics SDK before using A/B Testing "
          "SDK !");
      RETURN_IF_ERROR(true);
      return false;
    }
    LogInfo("Initialize SDK successfully.");
    instance_ = new Sdk(url_without_query, project_key);
  }
  return true;
}

json Sdk::FetchCacheABTest(const string &param_name,
                           const json &default_value) {
  if (instance_) {
    return instance_->consumer_->FetchExperimentResult(
        kFetchModeCache, param_name, default_value, FetchCompletionFunc,
        kDefaultTimeoutMilliseconds);
  }
  return default_value;
}

void Sdk::FastFetchABTest(const string &param_name, const json &default_value,
                          FetchHandler handler, int timeout_milliseconds) {
  if (instance_) {
    instance_->consumer_->FetchExperimentResult(kFetchModeFast, param_name,
                                                default_value, handler,
                                                timeout_milliseconds);
  }
}

void Sdk::AsyncFetchABTest(const string &param_name, const json &default_value,
                           FetchHandler handler, int timeout_milliseconds) {
  if (instance_) {
    instance_->consumer_->FetchExperimentResult(kFetchModeAsync, param_name,
                                                default_value, handler,
                                                timeout_milliseconds);
  }
}

Sdk::Sdk(const string &server_url, const string &project_key)
    : consumer_(new DefaultConsumer(server_url, project_key)) {
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
