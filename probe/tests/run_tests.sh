#!/usr/bin/env sh
# Сборка и запуск тестов ядра без Geode: нужен только компилятор C++20.
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/gddiff_probe_tests"
${CXX:-g++} -std=c++20 -O2 -Wall -Wextra -Werror -I src -o "$out" tests/test_core.cpp src/core/probe_core.cpp
"$out"
out2="${TMPDIR:-/tmp}/gddiff_solve_tests"
${CXX:-g++} -std=c++20 -O2 -Wall -Wextra -Werror -I src -o "$out2" tests/test_solve.cpp src/core/solve_core.cpp src/core/probe_core.cpp
"$out2"
out3="${TMPDIR:-/tmp}/gddiff_difficulty_tests"
${CXX:-g++} -std=c++20 -O2 -Wall -Wextra -Werror -I src -o "$out3" tests/test_difficulty.cpp src/core/difficulty.cpp
"$out3"
