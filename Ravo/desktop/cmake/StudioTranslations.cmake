# Compile and deploy every catalog declared by the versioned Studio locale manifest.

set(RAVO_STUDIO_LOCALE_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/i18n/locales.json")
set(RAVO_STUDIO_QM_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/i18n")
set(RAVO_STUDIO_I18N_CHECK_SCRIPT
  "${CMAKE_SOURCE_DIR}/Ravo/tools/check_i18n.py")

if(NOT TARGET Qt6::lrelease)
  message(FATAL_ERROR
    "Qt6::lrelease is required for Ravo Studio translations; install Qt LinguistTools.")
endif()
if(NOT EXISTS "${RAVO_STUDIO_LOCALE_MANIFEST}")
  message(FATAL_ERROR "Ravo Studio locale manifest is missing")
endif()
if(NOT EXISTS "${RAVO_STUDIO_I18N_CHECK_SCRIPT}")
  message(FATAL_ERROR
    "Ravo Studio i18n validator is missing: ${RAVO_STUDIO_I18N_CHECK_SCRIPT}")
endif()

file(READ "${RAVO_STUDIO_LOCALE_MANIFEST}" _ravo_locale_json)
string(JSON _ravo_locale_schema GET "${_ravo_locale_json}" schema)
if(NOT _ravo_locale_schema STREQUAL "ravo-studio-locales/v1")
  message(FATAL_ERROR "Unsupported Ravo Studio locale manifest schema")
endif()
string(JSON _ravo_locale_count LENGTH "${_ravo_locale_json}" locales)
if(_ravo_locale_count LESS 1)
  message(FATAL_ERROR "Ravo Studio locale manifest has no locales")
endif()

set(RAVO_STUDIO_LOCALES "")
set(RAVO_STUDIO_TS_FILES "")
math(EXPR _ravo_locale_last "${_ravo_locale_count} - 1")
foreach(_ravo_locale_index RANGE 0 ${_ravo_locale_last})
  string(JSON _ravo_locale_code GET "${_ravo_locale_json}"
    locales ${_ravo_locale_index} code)
  string(JSON _ravo_catalog_name GET "${_ravo_locale_json}"
    locales ${_ravo_locale_index} catalog)
  if(_ravo_locale_code IN_LIST RAVO_STUDIO_LOCALES)
    message(FATAL_ERROR "Duplicate Ravo Studio locale: ${_ravo_locale_code}")
  endif()
  get_filename_component(_ravo_catalog_basename "${_ravo_catalog_name}" NAME)
  if(NOT _ravo_catalog_basename STREQUAL _ravo_catalog_name OR
     NOT _ravo_catalog_name MATCHES "^RavoStudio_[A-Za-z_]+\\.ts$")
    message(FATAL_ERROR "Invalid Ravo Studio catalog name: ${_ravo_catalog_name}")
  endif()
  list(APPEND RAVO_STUDIO_LOCALES "${_ravo_locale_code}")
  list(APPEND RAVO_STUDIO_TS_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/i18n/${_ravo_catalog_name}")
endforeach()

file(GLOB _ravo_discovered_ts CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/i18n/RavoStudio_*.ts")
set(_ravo_expected_ts ${RAVO_STUDIO_TS_FILES})
list(SORT _ravo_discovered_ts)
list(SORT _ravo_expected_ts)
if(NOT _ravo_discovered_ts STREQUAL _ravo_expected_ts)
  message(FATAL_ERROR
    "Ravo Studio TS files must exactly match the locale manifest")
endif()

set(_ravo_i18n_check_args "")
foreach(_ravo_ts_file IN LISTS RAVO_STUDIO_TS_FILES)
  if(NOT EXISTS "${_ravo_ts_file}")
    message(FATAL_ERROR "Ravo Studio translation source is missing: ${_ravo_ts_file}")
  endif()
  list(APPEND _ravo_i18n_check_args --ts "${_ravo_ts_file}")
endforeach()

set(_ravo_i18n_validation_stamp
  "${CMAKE_CURRENT_BINARY_DIR}/studio_i18n_catalogs.validated")
add_custom_command(
  OUTPUT "${_ravo_i18n_validation_stamp}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${RAVO_STUDIO_QM_OUTPUT_DIR}"
  COMMAND "${Python3_EXECUTABLE}" "${RAVO_STUDIO_I18N_CHECK_SCRIPT}"
          --manifest "${RAVO_STUDIO_LOCALE_MANIFEST}" --require-all
          ${_ravo_i18n_check_args}
  COMMAND "${CMAKE_COMMAND}" -E touch "${_ravo_i18n_validation_stamp}"
  DEPENDS ${RAVO_STUDIO_TS_FILES} "${RAVO_STUDIO_LOCALE_MANIFEST}"
          "${RAVO_STUDIO_I18N_CHECK_SCRIPT}"
  COMMENT "Validating Ravo Studio translation catalogs"
  VERBATIM)

set(RAVO_STUDIO_QM_FILES "")
foreach(_ravo_ts_file IN LISTS RAVO_STUDIO_TS_FILES)
  get_filename_component(_ravo_ts_name "${_ravo_ts_file}" NAME_WE)
  set(_ravo_qm_file "${RAVO_STUDIO_QM_OUTPUT_DIR}/${_ravo_ts_name}.qm")
  add_custom_command(
    OUTPUT "${_ravo_qm_file}"
    COMMAND "$<TARGET_FILE:Qt6::lrelease>"
            -compress -nounfinished -removeidentical "${_ravo_ts_file}" -qm "${_ravo_qm_file}"
    DEPENDS "${_ravo_ts_file}" "${_ravo_i18n_validation_stamp}"
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

set(_ravo_locale_smoke_args "")
foreach(_ravo_locale IN LISTS RAVO_STUDIO_LOCALES)
  list(APPEND _ravo_locale_smoke_args --language "${_ravo_locale}")
endforeach()
add_custom_target(ravo_studio_localization_smoke
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Ravo/tools/smoke_ravo_studio.py"
          "$<TARGET_FILE:ravo_studio>" ${_ravo_locale_smoke_args}
  DEPENDS ravo_studio
  COMMENT "Smoke-loading every Ravo Studio locale"
  VERBATIM)

if(NOT APPLE)
  install(FILES ${RAVO_STUDIO_QM_FILES}
    DESTINATION "${CMAKE_INSTALL_BINDIR}/i18n")
endif()
