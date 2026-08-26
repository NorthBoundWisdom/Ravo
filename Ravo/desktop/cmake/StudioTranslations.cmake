# Compile and deploy the Ravo Studio translation catalogs.
#
# Source catalogs and translation memory are owned by
# .codex/skills/i18n-translation-workflow. This file only turns checked-in TS
# files into build-local QM output and installs/deploys that output.

set(RAVO_STUDIO_TS_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/i18n/RavoStudio_zh_CN.ts"
  "${CMAKE_CURRENT_SOURCE_DIR}/i18n/RavoStudio_en_US.ts")
set(RAVO_STUDIO_QM_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/i18n")
set(RAVO_STUDIO_I18N_CHECK_SCRIPT
  "${CMAKE_SOURCE_DIR}/Ravo/tools/check_i18n.py")

if(NOT TARGET Qt6::lrelease)
  message(FATAL_ERROR
    "Qt6::lrelease is required for Ravo Studio translations; install Qt LinguistTools.")
endif()
if(NOT EXISTS "${RAVO_STUDIO_I18N_CHECK_SCRIPT}")
  message(FATAL_ERROR
    "Ravo Studio i18n validator is missing: ${RAVO_STUDIO_I18N_CHECK_SCRIPT}")
endif()

set(RAVO_STUDIO_QM_FILES "")
foreach(_ravo_ts_file IN LISTS RAVO_STUDIO_TS_FILES)
  if(NOT EXISTS "${_ravo_ts_file}")
    message(FATAL_ERROR "Ravo Studio translation source is missing: ${_ravo_ts_file}")
  endif()
  get_filename_component(_ravo_ts_name "${_ravo_ts_file}" NAME_WE)
  set(_ravo_qm_file "${RAVO_STUDIO_QM_OUTPUT_DIR}/${_ravo_ts_name}.qm")

  add_custom_command(
    OUTPUT "${_ravo_qm_file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RAVO_STUDIO_QM_OUTPUT_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${RAVO_STUDIO_I18N_CHECK_SCRIPT}"
            --ts "${_ravo_ts_file}"
    COMMAND "$<TARGET_FILE:Qt6::lrelease>"
            -compress -nounfinished -removeidentical "${_ravo_ts_file}" -qm "${_ravo_qm_file}"
    DEPENDS "${_ravo_ts_file}" "${RAVO_STUDIO_I18N_CHECK_SCRIPT}"
    COMMENT "Generating ${_ravo_ts_name}.qm"
    VERBATIM)
  list(APPEND RAVO_STUDIO_QM_FILES "${_ravo_qm_file}")
endforeach()

add_custom_target(ravo_studio_translations DEPENDS ${RAVO_STUDIO_QM_FILES})
add_dependencies(ravo_studio ravo_studio_translations)
set_property(TARGET ravo_studio APPEND PROPERTY LINK_DEPENDS ${RAVO_STUDIO_QM_FILES})

if(APPLE)
  set(RAVO_STUDIO_RUNTIME_TRANSLATION_DIR
    "$<TARGET_BUNDLE_CONTENT_DIR:ravo_studio>/Resources/i18n")
else()
  set(RAVO_STUDIO_RUNTIME_TRANSLATION_DIR "$<TARGET_FILE_DIR:ravo_studio>/i18n")
endif()

add_custom_command(
  TARGET ravo_studio
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${RAVO_STUDIO_RUNTIME_TRANSLATION_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${RAVO_STUDIO_QM_OUTPUT_DIR}" "${RAVO_STUDIO_RUNTIME_TRANSLATION_DIR}"
  COMMENT "Deploying Ravo Studio translations"
  VERBATIM)

if(NOT APPLE)
  install(FILES ${RAVO_STUDIO_QM_FILES}
    DESTINATION "${CMAKE_INSTALL_BINDIR}/i18n")
endif()
