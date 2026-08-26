function(ravo_copy_windows_runtime_dlls target_name)
  if(NOT WIN32)
    return()
  endif()
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR
      "ravo_copy_windows_runtime_dlls: target does not exist: ${target_name}")
  endif()

  add_custom_command(TARGET "${target_name}" POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}"
      "-DDESTINATION_DIR=$<TARGET_FILE_DIR:${target_name}>"
      "-DTARGET_BINARY=$<TARGET_FILE:${target_name}>"
      "-DRUNTIME_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:${target_name}>,$<COMMA>>"
      "-DCONFIG=$<CONFIG>"
      -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/copy_runtime_dlls.cmake"
    COMMENT "Copying $<CONFIG> runtime DLLs for ${target_name}"
    VERBATIM)
endfunction()

function(ravo_deploy_windows_qt_runtime target_name)
  cmake_parse_arguments(PARSE_ARGV 1 DEPLOY "" "QML_DIR" "")
  if(NOT WIN32)
    return()
  endif()
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR
      "ravo_deploy_windows_qt_runtime: target does not exist: ${target_name}")
  endif()
  if(NOT DEPLOY_QML_DIR)
    message(FATAL_ERROR "ravo_deploy_windows_qt_runtime: QML_DIR is required")
  endif()

  set(_windeployqt_hints)
  if(TARGET Qt6::windeployqt)
    get_target_property(_windeployqt_location Qt6::windeployqt IMPORTED_LOCATION)
    if(_windeployqt_location)
      get_filename_component(_windeployqt_dir "${_windeployqt_location}" DIRECTORY)
      list(APPEND _windeployqt_hints "${_windeployqt_dir}")
    endif()
  endif()
  if(TARGET Qt6::qmake)
    get_target_property(_qmake_location Qt6::qmake IMPORTED_LOCATION)
    if(NOT _qmake_location)
      get_target_property(_qmake_location Qt6::qmake IMPORTED_LOCATION_RELEASE)
    endif()
    if(_qmake_location)
      get_filename_component(_qmake_bin_dir "${_qmake_location}" DIRECTORY)
      list(APPEND _windeployqt_hints "${_qmake_bin_dir}")
    endif()
  endif()
  if(DEFINED Qt6_DIR AND Qt6_DIR)
    get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    list(APPEND _windeployqt_hints "${_qt_prefix}/bin")
  endif()
  find_program(_windeployqt
    NAMES windeployqt windeployqt6 windeployqt.exe windeployqt6.exe
    HINTS ${_windeployqt_hints})
  if(NOT _windeployqt)
    message(FATAL_ERROR
      "ravo_deploy_windows_qt_runtime: windeployqt was not found under ${_windeployqt_hints}")
  endif()

  add_custom_command(TARGET "${target_name}" POST_BUILD
    COMMAND
      "${_windeployqt}"
      --verbose 0
      --qmldir "${DEPLOY_QML_DIR}"
      --no-translations
      --no-compiler-runtime
      --force
      --include-plugins qoffscreen
      "$<IF:$<CONFIG:Debug>,--debug,--release>"
      "$<TARGET_FILE:${target_name}>"
    COMMENT "Deploying $<CONFIG> Qt plugins for ${target_name}"
    VERBATIM)
endfunction()
