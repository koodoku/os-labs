#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
git pull
cmake -S project -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/hello
