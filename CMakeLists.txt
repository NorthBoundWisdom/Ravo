cmake_minimum_required(VERSION 3.26)

# Modern CMake knows which linkers rescan archives automatically. Let it
# remove repeated static-library entries on those platforms (notably Apple's
# linker) while preserving the repetitions required by traditional linkers.
if(POLICY CMP0156)
  cmake_policy(SET CMP0156 NEW)
endif()
if(POLICY CMP0179)
  cmake_policy(SET CMP0179 NEW)
endif()

# Ravo is the only buildable product. Frozen Darktable 0.9 lives in
# legacy/src/ as read-only reference and is not part of this graph.
project(Ravo VERSION 0.1.9 LANGUAGES CXX)
unset(PROJECT_VERSION CACHE)

option(BUILD_TESTING "Build Ravo unit and contract tests" OFF)
if(BUILD_TESTING)
  enable_testing()
endif()

add_subdirectory(Ravo)
