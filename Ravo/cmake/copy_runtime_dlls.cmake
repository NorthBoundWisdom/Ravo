# Copy the current-config Windows runtime DLL closure next to an executable.
# Debug and Release stay separate: $<TARGET_RUNTIME_DLLS> already selects the
# config-specific imported locations, and this script only searches those
# directories. vcpkg release `installed/<triplet>/bin` and debug
# `installed/<triplet>/debug/bin` are also filtered by CONFIG.

if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()

if(NOT DEFINED DESTINATION_DIR OR "${DESTINATION_DIR}" STREQUAL "")
  message(FATAL_ERROR "DESTINATION_DIR is required")
endif()

if(NOT DEFINED RUNTIME_DLLS OR "${RUNTIME_DLLS}" STREQUAL "")
  message(STATUS "No runtime DLLs to copy into ${DESTINATION_DIR}")
  return()
endif()

function(_ravo_runtime_dll_matches_config runtime_dll out_var)
  set(_matches TRUE)
  if(CONFIG STREQUAL "Release")
    if(runtime_dll MATCHES "[/\\\\]debug[/\\\\]bin[/\\\\]")
      set(_matches FALSE)
    endif()
  elseif(CONFIG STREQUAL "Debug")
    if(runtime_dll MATCHES "[/\\\\]installed[/\\\\][^/\\\\]+[/\\\\]bin[/\\\\]")
      set(_matches FALSE)
    endif()
  endif()
  set("${out_var}" "${_matches}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${DESTINATION_DIR}")
set(runtime_copy_lock "${DESTINATION_DIR}/.ravo-runtime-copy.lock")
file(LOCK "${runtime_copy_lock}" GUARD PROCESS TIMEOUT 600 RESULT_VARIABLE runtime_copy_lock_result)
if(NOT "${runtime_copy_lock_result}" STREQUAL "0")
  message(FATAL_ERROR
    "Unable to lock runtime deployment directory ${DESTINATION_DIR}: "
    "${runtime_copy_lock_result}")
endif()

string(REPLACE "," ";" runtime_dlls "${RUNTIME_DLLS}")
set(filtered_runtime_dlls)
foreach(runtime_dll IN LISTS runtime_dlls)
  _ravo_runtime_dll_matches_config("${runtime_dll}" _matches_config)
  if(_matches_config)
    list(APPEND filtered_runtime_dlls "${runtime_dll}")
  endif()
endforeach()
set(runtime_dlls "${filtered_runtime_dlls}")

set(runtime_search_dirs)
foreach(runtime_dll IN LISTS runtime_dlls)
  get_filename_component(runtime_dll_dir "${runtime_dll}" DIRECTORY)
  list(APPEND runtime_search_dirs "${runtime_dll_dir}")
endforeach()
list(REMOVE_DUPLICATES runtime_search_dirs)

if(WIN32 AND runtime_dlls)
  set(runtime_dependency_args)
  if(DEFINED TARGET_BINARY AND NOT "${TARGET_BINARY}" STREQUAL "")
    if(NOT EXISTS "${TARGET_BINARY}")
      message(FATAL_ERROR "TARGET_BINARY does not exist: ${TARGET_BINARY}")
    endif()
    list(APPEND runtime_dependency_args EXECUTABLES "${TARGET_BINARY}")
  endif()
  file(GET_RUNTIME_DEPENDENCIES
    ${runtime_dependency_args}
    LIBRARIES ${runtime_dlls}
    DIRECTORIES ${runtime_search_dirs}
    CONFLICTING_DEPENDENCIES_PREFIX conflicting_runtime_dlls
    PRE_EXCLUDE_REGEXES
      "^api-ms-"
      "^ext-ms-"
    POST_EXCLUDE_REGEXES
      "[/\\\\][Ww]indows[/\\\\]"
    RESOLVED_DEPENDENCIES_VAR resolved_runtime_dlls
    UNRESOLVED_DEPENDENCIES_VAR unresolved_runtime_dlls
  )
  foreach(runtime_dll IN LISTS resolved_runtime_dlls)
    _ravo_runtime_dll_matches_config("${runtime_dll}" _matches_config)
    if(_matches_config)
      list(APPEND runtime_dlls "${runtime_dll}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES runtime_dlls)
  if(unresolved_runtime_dlls)
    list(JOIN unresolved_runtime_dlls ", " unresolved_runtime_dlls_text)
    message(STATUS "Unresolved transitive runtime DLLs: ${unresolved_runtime_dlls_text}")
  endif()
endif()

foreach(runtime_dll IN LISTS runtime_dlls)
  if(NOT EXISTS "${runtime_dll}")
    message(FATAL_ERROR "Runtime DLL does not exist: ${runtime_dll}")
  endif()
  get_filename_component(runtime_dll_name "${runtime_dll}" NAME)
  file(COPY_FILE
    "${runtime_dll}"
    "${DESTINATION_DIR}/${runtime_dll_name}"
    ONLY_IF_DIFFERENT
  )
endforeach()
