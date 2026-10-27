cmake_minimum_required(VERSION 3.16)

set(identity_config "${CMAKE_CURRENT_LIST_DIR}/../cmake/AppIdentity.cmake")
include("${identity_config}")
if(NOT DISQUISITION_BUILD_CHANNEL STREQUAL "development")
  message(FATAL_ERROR "Local builds must default to development")
endif()
if(NOT DISQUISITION_BUILD_NUMBER STREQUAL "0")
  message(FATAL_ERROR "Local builds must not update themselves")
endif()

function(expect_identity channel name bundle settings)
  set(DISQUISITION_BUILD_CHANNEL "${channel}")
  include("${identity_config}")
  if(NOT DISQUISITION_APP_NAME STREQUAL name OR
     NOT DISQUISITION_BUNDLE_IDENTIFIER STREQUAL bundle OR
     NOT DISQUISITION_SETTINGS_NAME STREQUAL settings)
    message(FATAL_ERROR "Wrong app identity for ${channel}: ${DISQUISITION_APP_NAME}, ${DISQUISITION_BUNDLE_IDENTIFIER}, ${DISQUISITION_SETTINGS_NAME}")
  endif()
endfunction()

expect_identity(development "disquisition development"
  "com.disquisition.desktop.development" "Disquisition development")
expect_identity(staging "disquisition staging"
  "com.disquisition.desktop.staging" "Disquisition staging")
expect_identity(production "disquisition"
  "com.disquisition.desktop" "Disquisition")

# A typo must not silently create an app with production permissions or data.
foreach(invalid IN ITEMS "" "prod" "Development")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DDISQUISITION_BUILD_CHANNEL=${invalid}" -P "${identity_config}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT error MATCHES "Invalid DISQUISITION_BUILD_CHANNEL")
    message(FATAL_ERROR "Expected invalid channel '${invalid}' to fail: ${output}${error}")
  endif()
endforeach()

# A stamped build updates itself to the latest production release, so only
# production may carry a build number.
function(expect_build_number channel number expected_error)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DDISQUISITION_BUILD_CHANNEL=${channel}"
            "-DDISQUISITION_BUILD_NUMBER=${number}" -P "${identity_config}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(expected_error STREQUAL "")
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Expected ${channel} build ${number} to configure: ${output}${error}")
    endif()
  elseif(result EQUAL 0 OR NOT error MATCHES "${expected_error}")
    message(FATAL_ERROR "Expected ${channel} build '${number}' to fail: ${output}${error}")
  endif()
endfunction()

expect_build_number(production 42 "")
expect_build_number(development 0 "")
expect_build_number(staging 42 "only for production builds")
expect_build_number(development 42 "only for production builds")
foreach(invalid IN ITEMS "" "-1" "42abc" "main-42")
  expect_build_number(production "${invalid}" "Invalid DISQUISITION_BUILD_NUMBER")
endforeach()
