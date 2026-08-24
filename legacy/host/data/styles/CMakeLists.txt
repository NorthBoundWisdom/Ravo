#
# Install bundled example styles. Camera-specific style presets are not part
# of the DarkTableNext product baseline.
#
FILE(GLOB EXAMPLE_STYLE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/examples*.dtstyle)
install(FILES ${EXAMPLE_STYLE_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/styles COMPONENT DTApplication)
# Need to copy to have in build dir so we can run from there and have darktable find the
# files like after installing
file(COPY ${EXAMPLE_STYLE_FILES} DESTINATION ${DARKTABLE_DATADIR}/styles)
