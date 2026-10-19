cmake_minimum_required(VERSION 3.20)
project(maclauncher_android_arm32 LANGUAGES C CXX ASM)

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  message(FATAL_ERROR "The Android ARM32 adapter requires Apple Silicon")
endif()
if(NOT IS_DIRECTORY "${DYNARMIC_SOURCE}/src/dynarmic")
  message(FATAL_ERROR "Set DYNARMIC_SOURCE to the pinned Dynarmic checkout")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
option(ANDROID_A64_CPU "Build the A64 CPU binding alongside the A32 binding" OFF)
if(ANDROID_A64_CPU)
  set(DYNARMIC_FRONTENDS "A32;A64" CACHE STRING "" FORCE)
else()
  set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
endif()
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT ON CACHE BOOL "" FORCE)
# This bundled fmt release's consteval string subtraction is rejected by newer
# Apple Clang. Its ordinary formatter remains unchanged.
add_compile_definitions(FMT_CONSTEVAL=)
find_package(Boost 1.57 REQUIRED)
find_program(NODE_EXECUTABLE node REQUIRED)
add_subdirectory("${DYNARMIC_SOURCE}" "${CMAKE_BINARY_DIR}/dynarmic")
if(ANDROID_A64_CPU)
  add_library(android-arm64-cpu STATIC ../arm64/cpu.cpp)
  target_link_libraries(android-arm64-cpu PUBLIC dynarmic)
  target_compile_options(android-arm64-cpu PRIVATE -Wall -Wextra -Werror)
  target_include_directories(android-arm64-cpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../arm64")
  set(A64_ADDRESS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/a64_address_space.cpp")
  execute_process(COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../arm64/fallback-source.mjs"
    "${DYNARMIC_SOURCE}" "${A64_ADDRESS_SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ../arm64/fallback-source.mjs
    "${DYNARMIC_SOURCE}/src/dynarmic/backend/arm64/a64_address_space.cpp")
  get_target_property(A64_ORIGINAL_SOURCES dynarmic SOURCES)
  foreach(A64_ORIGINAL_SOURCE IN LISTS A64_ORIGINAL_SOURCES)
    if(A64_ORIGINAL_SOURCE MATCHES "backend/arm64/a64_address_space\\.cpp$")
      set_source_files_properties("${A64_ORIGINAL_SOURCE}" TARGET_DIRECTORY dynarmic PROPERTIES HEADER_FILE_ONLY TRUE)
    endif()
  endforeach()
  target_sources(dynarmic PRIVATE "${A64_ADDRESS_SOURCE}")
endif()
# Extend the existing code-cache owner for bounded native location queries.
# Generated instructions and original block maps remain upstream-owned.
set(DYNARMIC_CODE_LOCATION_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/code-location")
execute_process(COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/code-location-source.mjs"
  "${DYNARMIC_SOURCE}" "${DYNARMIC_CODE_LOCATION_DIRECTORY}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS code-location-source.mjs code-location.h
  "${DYNARMIC_SOURCE}/src/dynarmic/backend/arm64/address_space.cpp")
get_target_property(DYNARMIC_ORIGINAL_SOURCES dynarmic SOURCES)
foreach(DYNARMIC_ORIGINAL_SOURCE IN LISTS DYNARMIC_ORIGINAL_SOURCES)
  if(DYNARMIC_ORIGINAL_SOURCE MATCHES "backend/arm64/address_space\\.cpp$")
    set_source_files_properties("${DYNARMIC_ORIGINAL_SOURCE}" TARGET_DIRECTORY dynarmic PROPERTIES HEADER_FILE_ONLY TRUE)
  endif()
endforeach()
target_sources(dynarmic PRIVATE "${DYNARMIC_CODE_LOCATION_DIRECTORY}/address_space.cpp")
target_include_directories(dynarmic PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# Use the original page-table emitter for compact bounded native buffers too.
# Both VM bindings link this one CPU implementation and mapping owner.
set(DYNARMIC_PAGE_TABLE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/emit_arm64_memory.cpp")
execute_process(COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/page-table-source.mjs"
  "${DYNARMIC_SOURCE}" "${DYNARMIC_PAGE_TABLE_SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS page-table-source.mjs page-table.h
  "${DYNARMIC_SOURCE}/src/dynarmic/backend/arm64/emit_arm64_memory.cpp")
foreach(DYNARMIC_ORIGINAL_SOURCE IN LISTS DYNARMIC_ORIGINAL_SOURCES)
  # Dynarmic wraps architecture-specific sources in generated include units.
  # Retire that actual target entry so the static archive has one emitter.
  if(DYNARMIC_ORIGINAL_SOURCE MATCHES "backend/arm64/emit_arm64_memory\\.cpp$")
    set_source_files_properties("${DYNARMIC_ORIGINAL_SOURCE}" TARGET_DIRECTORY dynarmic PROPERTIES HEADER_FILE_ONLY TRUE)
  endif()
endforeach()
target_sources(dynarmic PRIVATE "${DYNARMIC_PAGE_TABLE_SOURCE}")
# Coalesce equivalent NEON mode transitions at the original FP status owner.
set(DYNARMIC_FPCR_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fpcr")
execute_process(COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fpcr-source.mjs"
  "${DYNARMIC_SOURCE}" "${DYNARMIC_FPCR_DIRECTORY}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS fpcr-source.mjs
  "${DYNARMIC_SOURCE}/src/dynarmic/backend/arm64/fpsr_manager.h"
  "${DYNARMIC_SOURCE}/src/dynarmic/backend/arm64/fpsr_manager.cpp"
  "${DYNARMIC_SOURCE}/src/dynarmic/backend/arm64/emit_arm64_vector_floating_point.cpp")
foreach(DYNARMIC_ORIGINAL_SOURCE IN LISTS DYNARMIC_ORIGINAL_SOURCES)
  if(DYNARMIC_ORIGINAL_SOURCE MATCHES "backend/arm64/(fpsr_manager|emit_arm64_vector_floating_point)\\.cpp$")
    set_source_files_properties("${DYNARMIC_ORIGINAL_SOURCE}" TARGET_DIRECTORY dynarmic PROPERTIES HEADER_FILE_ONLY TRUE)
  endif()
endforeach()
target_sources(dynarmic PRIVATE "${DYNARMIC_FPCR_DIRECTORY}/fpsr_manager.cpp"
  "${DYNARMIC_FPCR_DIRECTORY}/emit_arm64_vector_floating_point.cpp")
target_include_directories(dynarmic BEFORE PRIVATE "${DYNARMIC_FPCR_DIRECTORY}")
# Keep the upstream exception dispatcher intact while using the existing
# physical Android log owner instead of throwing FILE-based fmt diagnostics.
set(DYNARMIC_EXCEPTION_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/exception_handler_macos.cpp")
execute_process(COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/exception-source.mjs"
  "${DYNARMIC_SOURCE}" "${DYNARMIC_EXCEPTION_SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS exception-source.mjs code-location-source.mjs code-location.h
  "${DYNARMIC_SOURCE}/src/dynarmic/backend/exception_handler_macos.cpp")
set_source_files_properties("${DYNARMIC_SOURCE}/src/dynarmic/backend/exception_handler_macos.cpp"
  TARGET_DIRECTORY dynarmic PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(dynarmic PRIVATE "${DYNARMIC_EXCEPTION_SOURCE}")
set_source_files_properties("${DYNARMIC_EXCEPTION_SOURCE}" TARGET_DIRECTORY dynarmic
  PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(dynarmic INTERFACE "LINKER:-U,_klj_android_log_write")
add_library(android-arm32-cpu-objects OBJECT arm32.cpp)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/kernel-helpers.inc"
  COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/kernel-helpers.mjs"
    "${CMAKE_C_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/kernel-helpers.S" "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS kernel-helpers.S kernel-helpers.mjs VERBATIM)
target_sources(android-arm32-cpu-objects PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/kernel-helpers.inc")
target_link_libraries(android-arm32-cpu-objects PRIVATE dynarmic Boost::boost)
target_compile_options(android-arm32-cpu-objects PRIVATE -Wall -Wextra -Werror)
target_include_directories(android-arm32-cpu-objects PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
# Both VM bindings consume this one CPU/address-space implementation. It has
# no JNI, libc/service resolver or VM lifecycle dependency.
add_library(android-arm32-cpu STATIC $<TARGET_OBJECTS:android-arm32-cpu-objects>)
target_link_libraries(android-arm32-cpu PUBLIC dynarmic)
target_include_directories(android-arm32-cpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(android-arm32 SHARED $<TARGET_OBJECTS:android-arm32-cpu-objects>)
target_sources(android-arm32 PRIVATE ../proc-maps.c)
set_source_files_properties(../proc-maps.c PROPERTIES COMPILE_DEFINITIONS KLJ_PROC_MAPS_FORMAT_ONLY)
target_link_libraries(android-arm32 PRIVATE dynarmic Boost::boost)
target_compile_options(android-arm32 PRIVATE -Wall -Wextra -Werror)
target_include_directories(android-arm32 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(android-arm32 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(android-arm32 PROPERTIES INSTALL_NAME_DIR "@rpath")

# The selected JDK owns every JNI declaration. The adapter only changes pointer
# widths, AAPCS32 values and reference lifetimes; no Java APIs are substituted.
if(JDK_ROOT)
  find_program(NODE_EXECUTABLE node REQUIRED)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/jni-table.inc" "${CMAKE_CURRENT_BINARY_DIR}/jni-invocation.inc"
    COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/jni-generate.mjs"
      "${JDK_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS jni-generate.mjs "${JDK_ROOT}/include/jni.h"
    VERBATIM)
  target_sources(android-arm32 PRIVATE jni-bridge.cpp "${CMAKE_CURRENT_BINARY_DIR}/jni-table.inc" "${CMAKE_CURRENT_BINARY_DIR}/jni-invocation.inc")
  target_include_directories(android-arm32 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_include_directories(android-arm32 PUBLIC "${JDK_ROOT}/include" "${JDK_ROOT}/include/darwin")
  target_link_libraries(android-arm32 PRIVATE ffi)
endif()

# Native declarations come from both targets of the same published NDK. These
# files contain width/calling-convention metadata, never another service owner.
if(ARM32_IMPORTS_DIRECTORY)
  if(NOT JDK_ROOT OR NOT EXISTS "${ARM32_IMPORTS_DIRECTORY}/native-imports.inc")
    message(FATAL_ERROR "ARM32 native imports require the JDK and generated NDK declarations")
  endif()
  # Descriptor roles follow the adapter even when the original AST is reused.
  # Refresh preserves unchanged header timestamps, so this does not recompile
  # consumers unless their complete declarations actually changed.
  add_custom_target(android-arm32-imports
    COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/imports-generate.mjs"
      --refresh "${ARM32_IMPORTS_DIRECTORY}" VERBATIM)
  add_dependencies(android-arm32 android-arm32-imports)
  target_sources(android-arm32 PRIVATE native-imports.cpp runtime.cpp)
  target_sources(android-arm32 PRIVATE "${ARM32_FORMAT_DIRECTORY}/format-plan.cpp")
  target_include_directories(android-arm32 PRIVATE "${ARM32_IMPORTS_DIRECTORY}")
  target_include_directories(android-arm32 PRIVATE "${ARM32_FORMAT_DIRECTORY}")
  if(NOT EXISTS "${ARM32_SYSCALL_DIRECTORY}/syscalls.inc")
    message(FATAL_ERROR "ARM32 native imports require original syscall ABI metadata")
  endif()
  target_include_directories(android-arm32 PRIVATE "${ARM32_SYSCALL_DIRECTORY}")
endif()
