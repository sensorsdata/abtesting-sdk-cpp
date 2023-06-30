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

#pragma once
#ifndef SENSORS_ABTESTING_SDK_H_
#define SENSORS_ABTESTING_SDK_H_

#include <map>
#include <string>

#include "sensors_json.hpp"
#include "sensors_analytics_sdk.h"

#define ABTESTING_SDK_VERSION "0.0.2"
#define ABTESTING_SDK_NAME "Sensors A/B Testing CPP SDK"
#define ABTESTING_SDK_FULL_NAME ABTESTING_SDK_NAME " " ABTESTING_SDK_VERSION

namespace sensors_abtesting {
using namespace std;
using json = sensors_json::json;
class DefaultConsumer;

// 返回数据类型为 json，需要根据默认值类型显示获取返回内容
typedef std::function<void(const json &result)> FetchHandler;

// 自定义属性类
class ABTestPropertiesNode : public sensors_analytics::utils::ObjectNode {
 public:
  // 自定义属性只能传入字符串，List 需要转换
  void SetList(const string &property_name, const std::vector<string> &value);

 private:
  // 自定义属性不支持传入 Object
  void SetObject(const string &property_name, const ObjectNode &value);
};

class Sdk {
 public:
  // 初始化 A/B Testing SDK
  // server_url: 获取 A/B Testing 试验地址
  // experiment_file_path: 保存试验地址的文件路径
  static bool Init(const string &server_url, const string &experiment_file_path);

  // 总是从缓存获取试验
  // param_name: 试验参数名
  // default_value: 默认结果
  static json FetchCacheABTest(const string &param_name,
                               const json &default_value);

  // 异步从服务端获取最新试验结果，默认 timeout 为 30 秒
  // param_name: 试验参数名
  // default_value: 默认结果
  // handler: 返回试验结果的回调
  // timeout_milliseconds: 超时时间，默认为 30 秒
  static void AsyncFetchABTest(const string &param_name,
                               const json &default_value,
                               FetchHandler handler,
                               int timeout_milliseconds = 30000);

  // 优先从缓存获取试验结果，如果无缓存试验，则异步从网络请求
  // param_name: 试验参数名
  // default_value: 默认结果
  // handler: 返回试验结果的回调
  // timeout_milliseconds: 超时时间，默认为 30 秒
  static void FastFetchABTest(const string &param_name,
                              const json &default_value,
                              FetchHandler handler,
                              int timeout_milliseconds = 30000);
  
  // 异步从服务端获取最新试验结果，默认 timeout 为 30 秒
  // param_name: 试验参数名
  // default_value: 默认结果
  // properties: 自定义试验参数
  // handler: 返回试验结果的回调
  // timeout_milliseconds: 超时时间，默认为 30 秒
  static void AsyncFetchABTest(const string &param_name,
                               const json &default_value,
                               const ABTestPropertiesNode &properties,
                               FetchHandler handler,
                               int timeout_milliseconds = 30000);
  
  // 优先从缓存获取试验结果，如果无缓存试验，则异步从网络请求
  // param_name: 试验参数名
  // default_value: 默认结果
  // properties: 自定义试验参数
  // handler: 返回试验结果的回调
  // timeout_milliseconds: 超时时间，默认为 30 秒
  static void FastFetchABTest(const string &param_name,
                              const json &default_value,
                              const ABTestPropertiesNode &properties,
                              FetchHandler handler,
                              int timeout_milliseconds = 30000);

 private:
  Sdk(const string &server_url, const string &project_key, const string &experiment_file_path);

  ~Sdk();

  static Sdk *instance_;
  DefaultConsumer *consumer_;
};

}  // namespace sensors_abtesting

#endif  // SENSORS_ABTESTING_SDK_H_
