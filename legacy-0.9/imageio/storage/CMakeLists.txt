include_directories("${DARKTABLE_BINDIR}")

include(manage-symbol-visibility)

if(MSVC)
  add_compile_options("/FI${CMAKE_SOURCE_DIR}/src/common/module_api.h"
                      "/FI${CMAKE_SOURCE_DIR}/src/imageio/storage/imageio_storage_api.h")
else()
  add_definitions(-include common/module_api.h)
  add_definitions(-include imageio/storage/imageio_storage_api.h)
endif()

set(MODULES disk)

foreach(module ${MODULES})
	add_library(${module} MODULE "${module}.c")
endforeach(module)

foreach(module ${MODULES})
    target_link_libraries(${module} lib_darktable)
    dt_module_configure_link_dependencies(${module})
    set_target_properties(${module}
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/../../..)
    install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/imageio/storage COMPONENT DTApplication)
endforeach(module)
