if(MSVC)
  # /Wall includes compiler implementation details such as padding, automatic
  # inlining, and Spectre instrumentation notes. /W4 is the actionable project
  # warning baseline used for first-party C and C++ sources. Apply it per target
  # so FreeCM-managed dependencies retain their own warning policy.
  function(dt_target_enable_warnings target)
    target_compile_options(${target} PRIVATE
      /W4
      # GTK and module callbacks intentionally keep fixed signatures, even when
      # a specific implementation does not consume every argument.
      /wd4100
      # MSVC treats C restrict qualifiers as part of function pointer
      # compatibility and reports otherwise ABI-identical callback types.
      /wd4113
      # GLib callback storage and imported marshaller constants use the ABI
      # conversions prescribed by the library's C API.
      /wd4152
      /wd4232)

    if(NOT DT_MSVC_STRICT_CONVERSIONS)
      # GCC/Clang builds do not enable -Wconversion or -Wsign-conversion. Keep
      # the normal MSVC baseline equivalent and expose these broad diagnostics
      # through DT_MSVC_STRICT_CONVERSIONS for dedicated cleanup work.
      target_compile_options(${target} PRIVATE
        /wd4018
        /wd4244
        /wd4245
        /wd4267
        /wd4305
        /wd4389)
    endif()
  endfunction()
else()
  include(CheckCompilerFlagAndEnableIt)
  include(CheckCCompilerFlagAndEnableIt)
  include(CheckCXXCompilerFlagAndEnableIt)

  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wall)

  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wformat)
  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wformat-security)

  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wshadow)

  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wtype-limits)

  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wvla)

  CHECK_C_COMPILER_FLAG_AND_ENABLE_IT(-Wold-style-declaration)

  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wthread-safety)

  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wmaybe-uninitialized)

  # may be our bug :(
  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wno-error=varargs)

  # Fixed-size path buffers still produce conservative truncation diagnostics
  # with current GCC. Keep the diagnostic visible without making it fatal.
  CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wno-error=format-truncation)

  # needed to deal with warnings from some macOS SDK headers
  CHECK_C_COMPILER_FLAG_AND_ENABLE_IT(-Wno-typedef-redefinition)

  function(dt_target_enable_warnings target)
  endfunction()
endif()

# minimal main thread's stack/frame stack size.
# 2 MiB seems to work.
# MUST be a multiple of the system page size !!!
# see  $ getconf PAGESIZE
math(EXPR WANTED_STACK_SIZE 512*4*1024)

# minimal pthread stack/frame stack size.
# 2 MiB seems to work and is default on Linux.
# MUST be a multiple of the system page size !!!
# see  $ getconf PAGESIZE
math(EXPR WANTED_THREADS_STACK_SIZE 512*4*1024)

###### GTK+3 ######
#
#  Do not include individual headers
#
add_definitions(-DGTK_DISABLE_SINGLE_INCLUDES)

#
#  Do not use deprecated symbols
#
add_definitions(-DGDK_DISABLE_DEPRECATED)
add_definitions(-DGTK_DISABLE_DEPRECATED)
###### GTK+3 port ######
