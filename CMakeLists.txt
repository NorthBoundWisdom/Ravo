cmake_minimum_required(VERSION 3.26)

# Ravo is the only buildable product. Frozen Darktable 0.9 lives in
# legacy-0.9/ as read-only reference and is not part of this graph.
project(Ravo VERSION 0.1.0 LANGUAGES CXX)

add_subdirectory(Ravo)
