include_directories("${DARKTABLE_BINDIR}")

include_directories(SYSTEM "${PNG_PNG_INCLUDE_DIR}")

include(manage-symbol-visibility)

if(MSVC)
  add_compile_options("/FI${CMAKE_SOURCE_DIR}/src/common/module_api.h"
                      "/FI${CMAKE_SOURCE_DIR}/src/imageio/format/imageio_format_api.h")
else()
  add_definitions(-include common/module_api.h)
  add_definitions(-include imageio/format/imageio_format_api.h)
endif()

set(MODULES copy jpeg png tiff)

add_library(copy MODULE "copy.c")
add_library(jpeg MODULE "jpeg.c")
add_library(png MODULE "png.c")
add_library(tiff MODULE "tiff.c")

foreach(module ${MODULES})
    target_link_libraries(${module} PUBLIC lib_darktable)
    dt_module_configure_link_dependencies(${module})
    set_target_properties(${module}
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/../../..)
    install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/format COMPONENT DTApplication)
endforeach(module)
