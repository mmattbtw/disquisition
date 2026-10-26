cmake_minimum_required(VERSION 3.18)
project(disquisition_bundled_baresip LANGUAGES C)

if(NOT BARESIP_SOURCE_DIR OR NOT BARESIP_AUDIO_MODULE)
  message(FATAL_ERROR "Set BARESIP_SOURCE_DIR and BARESIP_AUDIO_MODULE")
endif()

find_package(OpenSSL REQUIRED)

set(STATIC ON CACHE BOOL "Link baresip modules into the executable" FORCE)
set(MODULES
  "account;g711;menu;mixminus;ctrl_tcp;${BARESIP_AUDIO_MODULE}"
  CACHE STRING "Baresip modules needed by Disquisition" FORCE)

add_subdirectory("${BARESIP_SOURCE_DIR}" baresip)

foreach(module IN LISTS MODULES)
  if(NOT TARGET ${module})
    message(FATAL_ERROR "Required baresip module ${module} was not built")
  endif()
endforeach()

# Baresip reads its options only when libre finds getopt.h, which MSVC lacks.
# Without them it ignores -f and never loads the profile the app writes.
if(MSVC)
  target_sources(baresip_exe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/getopt/getopt.c")
  target_include_directories(baresip_exe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/getopt")
  target_compile_definitions(baresip_exe PRIVATE HAVE_GETOPT)
endif()

# Baresip's FindRE module records the static libre archive but does not carry
# its OpenSSL link dependencies into the executable.
target_link_libraries(baresip_exe PRIVATE OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(baresip_exe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/output")
