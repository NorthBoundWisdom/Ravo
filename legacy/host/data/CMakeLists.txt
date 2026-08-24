add_subdirectory(pixmaps ${DARKTABLE_DATADIR}/pixmaps)
add_subdirectory(styles ${DARKTABLE_DATADIR}/styles)

add_subdirectory(kernels ${DARKTABLE_DATADIR}/kernels)

FILE(GLOB THEME_FILES "themes/*.css")
FILE(COPY ${THEME_FILES} DESTINATION "${DARKTABLE_DATADIR}/themes")
install(FILES ${THEME_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/themes COMPONENT DTApplication)

#
# Install watermarks
#
FILE(GLOB WATERMARKS "watermarks/*.svg")
FILE(COPY ${WATERMARKS} DESTINATION "${DARKTABLE_DATADIR}/watermarks")
install(FILES ${WATERMARKS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/watermarks COMPONENT DTApplication)

#
# Install the default shortcuts with extra key+mouse combinations.
#
FILE(COPY shortcutsrc DESTINATION "${DARKTABLE_DATADIR}")
install(FILES shortcutsrc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

#
# Install noiseprofiles
#
if(${VALIDATE_JSON})
  add_custom_target(
    validate_noiseprofiles_json ALL
    COMMAND ${jsonschema_BIN} -i ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.json ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.schema
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.json ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.schema
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of noiseprofiles.json"
  )
endif()
FILE(COPY noiseprofiles.json DESTINATION "${DARKTABLE_DATADIR}")
install(FILES noiseprofiles.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

#
# Install wb presets
#
if(${VALIDATE_JSON})
  add_custom_target(
    validate_wbpresets_json ALL
    COMMAND ${jsonschema_BIN} -i ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.json ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.schema
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.json ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.schema
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of wb_presets.json"
  )
endif()
FILE(COPY wb_presets.json DESTINATION "${DARKTABLE_DATADIR}")
install(FILES wb_presets.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

#
# Transform darktableconfig.xml into darktablerc
#

set(DEFCONFIG_APPLE "false")
set(DEFCONFIG_NONAPPLE "true")
set(DEFCONFIG_OPENCL "true")

if(APPLE)
  # OpenCL support on OS X isn't stable enough to enable it by default
  set(DEFCONFIG_OPENCL "false")
  set(DEFCONFIG_APPLE "true")
  set(DEFCONFIG_NONAPPLE "false")
endif(APPLE)


set(DEFCONFIG_AUDIOPLAYER "aplay")
if(APPLE)
  # OS X doesn't have aplay, but afplay should do
  set(DEFCONFIG_AUDIOPLAYER "afplay")
endif(APPLE)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/darktableconfig.dtd ${DARKTABLE_DATADIR}/darktableconfig.dtd COPYONLY)

# get_property(DT_PLUGIN_IOPS GLOBAL PROPERTY DT_PLUGIN_IOPS)
get_property(DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT GLOBAL PROPERTY DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT)

set(DARKTABLECONFIG_IOP_ENTRIES "")
foreach(DT_PLUGIN_IOP IN LISTS DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT)
  set(DARKTABLECONFIG_IOP_ENTRIES "${DARKTABLECONFIG_IOP_ENTRIES}
  <dtconfig>
    <name>plugins/darkroom/${DT_PLUGIN_IOP}/visible</name>
    <type>bool</type>
    <default>true</default>
    <shortdescription/>
    <longdescription/>
  </dtconfig>")
endforeach()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/darktableconfig.xml.in ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml)

add_custom_target(
  validate_darktableconfig_xml ALL
  COMMAND ${Xmllint_BIN} --nonet --valid --noout ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
  COMMENT "Checking validity of ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml"
)

add_custom_command(
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
  COMMAND ${Xsltproc_BIN} --nonet ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
)

add_custom_target(
  darktablerc_file ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
)
add_dependencies(darktablerc_file validate_darktableconfig_xml)

# first of all install darktablerc file into share as template
# postinst script should copy this into users homedirectory
# $HOME/.config/darktable/darktable.rc
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktablerc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)
