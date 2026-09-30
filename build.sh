#!/bin/bash
# Build du solveur sur la carte (Orange Pi / Linux)
# Usage: ./build.sh
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
echo "compilation..."
$CXX -O3 -march=native -std=c++20 -pthread -Wall -Wextra main.cpp -o solver2048
echo "build OK -> ./solver2048"
./solver2048 --selftest
echo "selftest OK"
