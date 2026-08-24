include_directories("${DARKTABLE_BINDIR}/")

include(manage-symbol-visibility)

if(MSVC)
  add_compile_options("/FI${CMAKE_SOURCE_DIR}/src/common/module_api.h"
                      "/FI${CMAKE_SOURCE_DIR}/src/views/view_api.h")
else()
  add_definitions(-include common/module_api.h)
  add_definitions(-include views/view_api.h)
endif()

set(MODULES darkroom lighttable)

add_library(darkroom MODULE "darkroom.c")
add_library(lighttable MODULE "lighttable.c")
foreach(module ${MODULES})
    target_link_libraries(${module} lib_darktable)
    dt_module_configure_link_dependencies(${module})
endforeach(module)

foreach(module ${MODULES})
    set_target_properties(${module}
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/..)

    install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/views COMPONENT DTApplication)
endforeach(module)
