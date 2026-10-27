# Local builds stay separate from downloaded staging and production apps,
# regardless of the checkout's branch or CMAKE_BUILD_TYPE.
set(DISQUISITION_BUILD_CHANNEL "development" CACHE STRING
    "Desktop app channel: development, staging, or production")
set_property(CACHE DISQUISITION_BUILD_CHANNEL PROPERTY STRINGS development staging production)

if(DISQUISITION_BUILD_CHANNEL STREQUAL "production")
  set(DISQUISITION_APP_NAME "disquisition")
  set(DISQUISITION_BUNDLE_IDENTIFIER "com.disquisition.desktop")
  # Preserve existing production preferences and application-data paths.
  set(DISQUISITION_SETTINGS_NAME "Disquisition")
elseif(DISQUISITION_BUILD_CHANNEL STREQUAL "development" OR
       DISQUISITION_BUILD_CHANNEL STREQUAL "staging")
  set(DISQUISITION_APP_NAME "disquisition ${DISQUISITION_BUILD_CHANNEL}")
  set(DISQUISITION_BUNDLE_IDENTIFIER "com.disquisition.desktop.${DISQUISITION_BUILD_CHANNEL}")
  set(DISQUISITION_SETTINGS_NAME "Disquisition ${DISQUISITION_BUILD_CHANNEL}")
else()
  message(FATAL_ERROR
    "Invalid DISQUISITION_BUILD_CHANNEL '${DISQUISITION_BUILD_CHANNEL}'; choose development, staging, or production")
endif()

set(DISQUISITION_BUILD_NUMBER "0" CACHE STRING
    "Release build number the desktop app updates from; 0 turns updates off")
set(DISQUISITION_UPDATE_REPOSITORY "mmattbtw/disquisition" CACHE STRING
    "GitHub repository whose latest release the desktop app updates to")
if(NOT DISQUISITION_BUILD_NUMBER MATCHES "^[0-9]+$")
  message(FATAL_ERROR
    "Invalid DISQUISITION_BUILD_NUMBER '${DISQUISITION_BUILD_NUMBER}'; use a workflow run number, or 0")
endif()
if(NOT DISQUISITION_BUILD_NUMBER EQUAL 0 AND NOT DISQUISITION_BUILD_CHANNEL STREQUAL "production")
  message(FATAL_ERROR
    "DISQUISITION_BUILD_NUMBER is only for production builds, not ${DISQUISITION_BUILD_CHANNEL}")
endif()
