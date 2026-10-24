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
