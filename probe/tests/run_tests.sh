#!/usr/bin/env sh
# Сборка и запуск тестов ядра без Geode: нужен только компилятор C++20.
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/gddiff_probe_tests"
${CXX:-g++} -std=c++20 -O2 -Wall -Wextra -Werror -I src -o "$out" tests/test_core.cpp src/core/probe_core.cpp
"$out"
