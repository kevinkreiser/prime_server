#!/usr/bin/env bash
set -e

OS=$(uname)
N=1
if [[ ${OS} == "Linux" ]]; then
  N=$(nproc)
elif [[ ${OS} == "Darwin" ]]; then
  N=$(sysctl -n hw.physicalcpu)
fi

clang-format --version
find src prime_server test -type f -name '*.hpp' -o -name '*.cpp' | grep -vE "prime_server/logging|test/testing" | xargs -I{} -P ${N} clang-format -i -style=file {}
