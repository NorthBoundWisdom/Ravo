add_library(ravo_recipe STATIC)

target_sources(ravo_recipe
  PRIVATE
    src/operation.cpp
    src/recipe.cpp
    src/develop.cpp)

target_include_directories(ravo_recipe
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_recipe PUBLIC ravo_foundation)

ravo_configure_target(ravo_recipe)
