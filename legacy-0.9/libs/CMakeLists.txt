include_directories("${DARKTABLE_BINDIR}/")

include(manage-symbol-visibility)

if(MSVC)
  add_compile_options("/FI${CMAKE_SOURCE_DIR}/src/common/module_api.h"
                      "/FI${CMAKE_SOURCE_DIR}/src/libs/lib_api.h")
else()
  add_definitions(-include common/module_api.h)
  add_definitions(-include libs/lib_api.h)
endif()

# The modules
set(MODULES import export copy_history styles tagging image select recentcollect filtering metadata metadata_view navigation histogram history snapshots modulegroups backgroundjobs masks ioporder)

# The tools
set(MODULES ${MODULES} viewswitcher)
set(MODULES ${MODULES} darktable_label)
set(MODULES ${MODULES} flags)
set(MODULES ${MODULES} ratings)
set(MODULES ${MODULES} colorlabels)
set(MODULES ${MODULES} lighttable_mode)
set(MODULES ${MODULES} view_toolbox)
set(MODULES ${MODULES} module_toolbox)
set(MODULES ${MODULES} filmstrip)
set(MODULES ${MODULES} hinter)
set(MODULES ${MODULES} image_infos)

# Modules are discovered by scanning this directory at runtime. Remove the
# old binaries during configure so an incremental build cannot load deleted
# Lighttable modules.
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/libtimeline${CMAKE_SHARED_MODULE_SUFFIX}")
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/liblog_history${CMAKE_SHARED_MODULE_SUFFIX}")
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/libcollect${CMAKE_SHARED_MODULE_SUFFIX}")
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/libcolorpicker${CMAKE_SHARED_MODULE_SUFFIX}")
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/libduplicate${CMAKE_SHARED_MODULE_SUFFIX}")
if(APPLE)
  file(REMOVE "${DARKTABLE_BINDIR}/darktable.app/Contents/Resources/lib/darktable/plugins/lighttable/libcollect${CMAKE_SHARED_MODULE_SUFFIX}")
  file(REMOVE "${DARKTABLE_BINDIR}/darktable.app/Contents/Resources/lib/darktable/plugins/lighttable/libcolorpicker${CMAKE_SHARED_MODULE_SUFFIX}")
  file(REMOVE "${DARKTABLE_BINDIR}/darktable.app/Contents/Resources/lib/darktable/plugins/lighttable/libduplicate${CMAKE_SHARED_MODULE_SUFFIX}")
endif()

# modules
add_library(import MODULE "import.c")
add_library(export MODULE "export.c" "export_metadata.c")
add_library(copy_history MODULE "copy_history.c")
add_library(styles MODULE "styles.c")
add_library(image MODULE "image.c")
add_library(tagging MODULE "tagging.c")
add_library(select MODULE "select.c")
add_library(recentcollect MODULE "recentcollect.c")
add_library(filtering MODULE "filtering.c")
add_library(metadata MODULE "metadata.c")
add_library(metadata_view MODULE "metadata_view.c")
add_library(navigation MODULE "navigation.c")
add_library(histogram MODULE "histogram.c" "scopes/histogram.c" "scopes/waveform.c" "scopes/vectorscope.c" "scopes/split.c")
add_library(history MODULE "history.c")
add_library(snapshots MODULE "snapshots.c")
add_library(modulegroups MODULE "modulegroups.c")
add_library(backgroundjobs MODULE "backgroundjobs.c")
add_library(masks MODULE "masks.c")
add_library(ioporder MODULE "ioporder.c")

# tools
add_library(viewswitcher MODULE "tools/viewswitcher.c")

add_library(darktable_label MODULE "tools/darktable.c")

add_library(flags MODULE "tools/flags.c")
add_library(colorlabels MODULE "tools/colorlabels.c")
add_library(ratings MODULE "tools/ratings.c")
add_library(lighttable_mode MODULE "tools/lighttable.c")
add_library(view_toolbox MODULE "tools/view_toolbox.c")
add_library(module_toolbox MODULE "tools/module_toolbox.c")
add_library(filmstrip MODULE "tools/filmstrip.c")
add_library(hinter MODULE "tools/hinter.c")
add_library(image_infos MODULE "tools/image_infos.c")

# Add libs references
foreach(module ${MODULES})
  target_link_libraries(${module} lib_darktable)
  dt_module_configure_link_dependencies(${module})
endforeach(module)

# Add installation target for each module
# TODO: Modules are now not only lighttable related and should be placed
#       in a better matching structure then just in lighttable directory.
foreach(module ${MODULES})
  if(NOT WIN32)
    set_target_properties(${module}
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/../..)
  endif()
  install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/lighttable COMPONENT DTApplication)
endforeach(module)
