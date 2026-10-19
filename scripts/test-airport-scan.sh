#!/bin/sh
# Native scan wire/bounds/security checks, without loading either kext.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/test
clang++ -std=c++14 -g -fsanitize=address,undefined tests/airport_scan_test.cpp \
    -o build/test/airport_scan_test
build/test/airport_scan_test

# Radio power worker: deterministic concurrent request during a slow OFF.
clang++ -std=c++14 -pthread -g -fsanitize=address,undefined tests/airport_power_test.cpp \
    -o build/test/airport_power_test
build/test/airport_power_test
