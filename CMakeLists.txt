cmake_minimum_required(VERSION 3.26)

# Ravo is the only buildable product. Frozen Darktable 0.9 lives in
# legacy/src/ as read-only reference and is not part of this graph.
project(Ravo VERSION 0.1.4 LANGUAGES CXX)
unset(PROJECT_VERSION CACHE)

option(BUILD_TESTING "Build Ravo unit and contract tests" OFF)
if(BUILD_TESTING)
  enable_testing()
endif()

add_subdirectory(Ravo)
