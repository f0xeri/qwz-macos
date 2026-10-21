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

# Native join bounds, PMK handling and association completion.
clang++ -std=c++14 -g -fsanitize=address,undefined tests/airport_join_test.cpp \
    -o build/test/airport_join_test
build/test/airport_join_test

# Current-BSS response required by WCL before accepting a completed join.
clang++ -std=c++14 -g -fsanitize=address,undefined tests/airport_extended_bss_test.cpp \
    -o build/test/airport_extended_bss_test
build/test/airport_extended_bss_test

# airportd's post-join CHANNEL request must report actual negotiated metadata.
clang++ -std=c++14 -g -fsanitize=address,undefined tests/airport_current_link_test.cpp \
    -o build/test/airport_current_link_test
build/test/airport_current_link_test
