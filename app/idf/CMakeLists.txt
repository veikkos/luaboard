# ESP-IDF project for the real device (ESP-IDF v6.0.3).
# Build from the repo root: make toolchain, make fw, make flash PORT=...
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(luaboard)
