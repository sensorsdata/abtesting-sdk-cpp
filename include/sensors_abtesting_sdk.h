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

#define ABTESTING_SDK_VERSION "0.0.1"
#define ABTESTING_SDK_NAME "Sensors A/B Testing CPP SDK"
#define ABTESTING_SDK_FULL_NAME ABTESTING_SDK_NAME " " ABTESTING_SDK_VERSION

namespace sensors_abtesting {
using namespace std;
using json = sensors_json::json;
class DefaultConsumer;

// 返回数据类型为 json，需要根据默认值类型显示获取返回内容
typedef std::function<void(const json &result)> FetchHandler;

class Sdk {
 public:
  static bool Init(const string &server_url);

  static json FetchCacheABTest(const string &param_name,
                               const json &default_value);

  static void AsyncFetchABTest(const string &param_name,
                               const json &default_value, FetchHandler handler,
                               int timeout_milliseconds = 30000);

  static void FastFetchABTest(const string &param_name,
                              const json &default_value, FetchHandler handler,
                              int timeout_milliseconds = 30000);

 private:
  Sdk(const string &server_url, const string &project_key);

  ~Sdk();

  static Sdk *instance_;
  DefaultConsumer *consumer_;
};

}  // namespace sensors_abtesting

#endif  // SENSORS_ABTESTING_SDK_H_
