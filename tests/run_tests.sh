#!/bin/bash
set -euo pipefail

# 使用客户实际依赖的埋点 SDK 源码；测试仅替换 HTTP，不连接后台。
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
sa_dir="${1:-${SENSORS_ANALYTICS_SDK_DIR:-}}"
if [[ -z "$sa_dir" || ! -f "$sa_dir/src/sensors_analytics_sdk.cpp" ]]; then
  echo "用法：tests/run_tests.sh /path/to/sa-sdk-cpp（推荐 v1.0.3）" >&2
  exit 2
fi
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/abtesting-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
flags=(-std=c++11 -pthread -g -Wall -Wextra)
if [[ -n "${ABTEST_SANITIZERS:-}" ]]; then
  flags+=("-fsanitize=$ABTEST_SANITIZERS" -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${flags[@]}" -I"$repo_dir/include" -I"$sa_dir/include" \
  "$repo_dir/tests/abtesting_test.cpp" \
  "$sa_dir/src/sensors_analytics_sdk.cpp" "$sa_dir/src/sensors_utils.cpp" \
  -lz -o "$build_dir/abtesting_test"
"$build_dir/abtesting_test" "$build_dir"
