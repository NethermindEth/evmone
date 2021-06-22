#!/bin/bash

RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${RED}Setting environment${NC}"
source /opt/intel/sgxsdk/environment

echo -e "${RED}Configure Project${NC}"
cmake -S . -B build -DEVMONE_TESTING=ON

echo -e "${RED}Building Project${NC}"
cmake --build build --parallel

echo -e "${RED}Running Unit Tests${NC}"
build/bin/evmone-unittests

echo -e "${RED}Running Benchmarks${NC}"
build/bin/evmone-bench test/benchmarks

