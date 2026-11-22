cmake_minimum_required(VERSION 3.16)

# Explicit version: the build then works from a zip download or a repository
# without commits (ESP-IDF otherwise asks git for a version).
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sting)
