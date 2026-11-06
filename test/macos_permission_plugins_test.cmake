cmake_minimum_required(VERSION 3.16)

# Check the shipped executable, rather than a test executable with different
# plugin imports. Do not request camera/microphone access or change TCC state.
execute_process(COMMAND "${NM}" "${APP_BINARY}"
  RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot inspect app permission plugins: ${error}")
endif()
foreach(permission IN ITEMS Camera Microphone)
  if(NOT symbols MATCHES "qt_static_plugin_QDarwin${permission}PermissionPlugin")
    message(FATAL_ERROR "App is missing the Qt ${permission} permission plugin")
  endif()
  execute_process(COMMAND /usr/libexec/PlistBuddy
    -c "Print :NS${permission}UsageDescription" "${APP_PLIST}"
    RESULT_VARIABLE result OUTPUT_VARIABLE description ERROR_VARIABLE error)
  if(NOT result EQUAL 0 OR description STREQUAL "")
    message(FATAL_ERROR "App is missing NS${permission}UsageDescription: ${error}")
  endif()
endforeach()
