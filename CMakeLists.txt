cmake_minimum_required(VERSION 3.16)
project(disquisition LANGUAGES CXX)

option(BUILD_DESKTOP_APP "Build the Qt desktop chat and voice app" ON)
option(BUILD_RELAY "Build the community relay" ON)
include(cmake/AppIdentity.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)
include(CTest)
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# SQLite is only used for local desktop message saving and its tests. A
# relay-only build needs Qt Core/Network and OpenSSL, with no database.
if(BUILD_DESKTOP_APP OR BUILD_TESTING)
  if(APPLE)
    find_program(HOMEBREW_EXECUTABLE brew)
    if(HOMEBREW_EXECUTABLE)
      execute_process(COMMAND "${HOMEBREW_EXECUTABLE}" --prefix sqlite
        OUTPUT_VARIABLE HOMEBREW_SQLITE_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      if((NOT DEFINED SQLite3_LIBRARY OR
          SQLite3_LIBRARY MATCHES "MacOSX[.]sdk|CommandLineTools") AND
         EXISTS "${HOMEBREW_SQLITE_PREFIX}/include/sqlite3.h" AND
         EXISTS "${HOMEBREW_SQLITE_PREFIX}/lib/libsqlite3.dylib")
        set(SQLite3_INCLUDE_DIR "${HOMEBREW_SQLITE_PREFIX}/include" CACHE PATH "SQLite3 headers" FORCE)
        set(SQLite3_LIBRARY "${HOMEBREW_SQLITE_PREFIX}/lib/libsqlite3.dylib" CACHE FILEPATH "SQLite3 library" FORCE)
      endif()
    endif()
  endif()
  find_package(SQLite3 3.24 REQUIRED)
  if(TARGET SQLite3::SQLite3)
    set(SQLITE_TARGET SQLite3::SQLite3)
  else()
    set(SQLITE_TARGET SQLite::SQLite3)
  endif()
  add_library(chat_recent_messages STATIC src/common/recent_messages.cpp)
  target_include_directories(chat_recent_messages PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(chat_recent_messages PRIVATE ${SQLITE_TARGET})
  if(BUILD_TESTING)
    add_executable(recent_messages_test test/recent_messages_test.cpp)
    target_link_libraries(recent_messages_test PRIVATE chat_recent_messages ${SQLITE_TARGET})
    add_test(NAME recent_messages_test COMMAND recent_messages_test)
  endif()
endif()

add_library(chat_communities STATIC
  include/community/node.h
  src/community/identity.cpp src/community/model.cpp src/community/store.cpp src/community/node.cpp)
target_include_directories(chat_communities PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(chat_communities PUBLIC Qt6::Core Qt6::Network PRIVATE OpenSSL::Crypto)
set_target_properties(chat_communities PROPERTIES AUTOMOC ON)

if(BUILD_RELAY OR BUILD_TESTING)
  add_library(chat_community_router STATIC
    include/relay/community_router.h src/relay/community_router.cpp)
  target_link_libraries(chat_community_router PUBLIC chat_communities)
  set_target_properties(chat_community_router PROPERTIES AUTOMOC ON)
endif()
if(BUILD_RELAY)
  add_executable(relay src/relay/main.cpp src/relay/relay.cpp)
  target_link_libraries(relay PRIVATE chat_community_router)
endif()

if(BUILD_TESTING)
  add_test(NAME app_identity_test
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/app_identity_test.cmake")
  add_test(NAME macos_dmg_retry_test
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test/macos_dmg_retry_test.sh")
  if(UNIX)
    add_test(NAME hdiutil_retry_test
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test/hdiutil_retry_test.sh")
  endif()
  add_test(NAME macos_codesign_test
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/macos_codesign_test.cmake")
  add_executable(community_model_test test/community_model_test.cpp)
  target_link_libraries(community_model_test PRIVATE chat_communities)
  add_test(NAME community_model_test COMMAND community_model_test)
  set_tests_properties(community_model_test PROPERTIES TIMEOUT 120)
  add_executable(community_network_test test/community_network_test.cpp)
  target_link_libraries(community_network_test PRIVATE chat_communities chat_community_router)
  set(COMMUNITY_RELAY_TEST_ARGS)
  if(TARGET relay)
    add_dependencies(community_network_test relay)
    list(APPEND COMMUNITY_RELAY_TEST_ARGS "$<TARGET_FILE:relay>")
  endif()
  add_test(NAME community_network_test COMMAND community_network_test ${COMMUNITY_RELAY_TEST_ARGS})
  set_tests_properties(community_network_test PROPERTIES TIMEOUT 120)
  if(APPLE OR WIN32)
    if(APPLE)
      set(COMMUNITY_NATIVE_TLS_BACKEND securetransport)
    else()
      set(COMMUNITY_NATIVE_TLS_BACKEND schannel)
    endif()
    add_test(NAME community_native_tls_test COMMAND community_network_test ${COMMUNITY_RELAY_TEST_ARGS})
    set_tests_properties(community_native_tls_test PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      ENVIRONMENT "DISQUISITION_TEST_TLS_BACKEND=${COMMUNITY_NATIVE_TLS_BACKEND}")
  endif()
endif()

if(BUILD_DESKTOP_APP)
  if(APPLE)
    enable_language(OBJCXX)
  endif()
  # Linux desktop notifications go over D-Bus when Qt has it.
  find_package(Qt6 6.6 COMPONENTS Widgets Network Multimedia OPTIONAL_COMPONENTS DBus QUIET)
  # Screen sharing encodes and decodes H.264 with FFmpeg.
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavcodec>=60 libavutil libswscale)
  endif()
  if(Qt6_FOUND AND FFMPEG_FOUND)
    # Keep the barcode dependency static without changing the app's other
    # dependency defaults. Pinned source supports both QR export and import.
    function(configure_community_qr)
      set(BUILD_SHARED_LIBS OFF)
      set(ZXING_READERS ON CACHE BOOL "" FORCE)
      set(ZXING_WRITERS OLD CACHE STRING "" FORCE)
      set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(community_zxing
        GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
        GIT_TAG d6068bcebeb8fd9f0d35a99b00d202be86a14dbe)
      FetchContent_MakeAvailable(community_zxing)
    endfunction()
    configure_community_qr()
    add_library(community_qr STATIC src/community/qr.cpp)
    target_link_libraries(community_qr PUBLIC chat_communities Qt6::Gui PRIVATE ZXing::ZXing)
    if(BUILD_TESTING)
      add_executable(community_qr_test test/community_qr_test.cpp)
      target_link_libraries(community_qr_test PRIVATE community_qr)
      add_test(NAME community_qr_test COMMAND community_qr_test)
    endif()
    # GIF search, media links and image transfer, kept apart from the
    # widgets so they can be tested without a display.
    add_library(desktop_media STATIC
      include/desktop/gif_search.h
      include/desktop/image_transfer.h
      include/desktop/remote_media.h
      src/desktop/gif_search.cpp
      src/desktop/image_transfer.cpp
      src/desktop/remote_media.cpp)
    set_target_properties(desktop_media PROPERTIES AUTOMOC ON)
    target_include_directories(desktop_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(desktop_media PUBLIC Qt6::Gui Qt6::Network)

    add_library(chat_drawing STATIC src/common/drawing_protocol.cpp)
    target_include_directories(chat_drawing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(BUILD_TESTING)
      add_executable(drawing_protocol_test test/drawing_protocol_test.cpp)
      target_link_libraries(drawing_protocol_test PRIVATE chat_drawing)
      add_test(NAME drawing_protocol_test COMMAND drawing_protocol_test)
    endif()

    # Kept apart from the widgets so it can be tested without a display.
    add_library(desktop_notifications STATIC
      include/desktop/mentions.h
      include/desktop/notification_rules.h
      include/desktop/unread_badge.h
      src/desktop/mentions.cpp
      src/desktop/notification_rules.cpp
      src/desktop/unread_badge.cpp)
    target_include_directories(desktop_notifications PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(desktop_notifications PUBLIC Qt6::Gui)

    add_library(desktop_screen STATIC
      include/desktop/screen_drawing.h
      include/desktop/screen_drawing_session.h
      include/desktop/screen_stage.h
      include/desktop/share_source.h
      src/desktop/screen_drawing.cpp
      src/desktop/screen_drawing_session.cpp
      src/desktop/screen_stage.cpp
      src/desktop/share_source.cpp)
    set_target_properties(desktop_screen PROPERTIES AUTOMOC ON)
    target_link_libraries(desktop_screen PUBLIC chat_drawing Qt6::Widgets Qt6::Multimedia)
    if(UNIX AND NOT APPLE)
      pkg_check_modules(XCB QUIET IMPORTED_TARGET xcb)
      if(XCB_FOUND)
        target_link_libraries(desktop_screen PRIVATE PkgConfig::XCB)
        target_compile_definitions(desktop_screen PRIVATE DISQUISITION_HAVE_XCB)
      endif()
    endif()
    if(WIN32)
      target_link_libraries(desktop_screen PRIVATE dwmapi user32)
    endif()

    # The headers are listed so AUTOMOC finds the Q_OBJECT classes in them.
    add_executable(disquisition-desktop MACOSX_BUNDLE
      include/desktop/audio_permission.h
      include/desktop/chat_media.h
      include/desktop/desktop_notifier.h
      include/desktop/gif_picker.h
      include/desktop/push_to_talk.h
      include/desktop/main_window.h
      include/desktop/mention_completer.h
      include/desktop/notification_center.h
      include/desktop/relay_audio.h
      include/desktop/screen_capture.h
      include/desktop/screen_h264.h
      include/desktop/share_picker.h
      src/desktop/audio_permission.cpp
      src/desktop/chat_media.cpp
      src/desktop/desktop_notifier.cpp
      src/desktop/gif_picker.cpp
      src/desktop/main.cpp
      src/desktop/push_to_talk.cpp
      src/desktop/main_window.cpp
      src/desktop/mention_completer.cpp
      src/desktop/notification_center.cpp
      src/desktop/relay_audio.cpp
      src/desktop/screen_capture.cpp
      src/desktop/screen_h264.cpp
      src/desktop/share_picker.cpp)
    set_target_properties(disquisition-desktop PROPERTIES AUTOMOC ON)
    target_sources(disquisition-desktop PRIVATE
      include/desktop/community_panel.h src/desktop/community_panel.cpp)
    target_link_libraries(disquisition-desktop PRIVATE chat_communities community_qr)
    configure_file(cmake/app_identity.h.in generated/desktop/app_identity.h @ONLY)
    target_include_directories(disquisition-desktop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(disquisition-desktop PRIVATE
      chat_recent_messages desktop_media desktop_notifications desktop_screen
      Qt6::Widgets Qt6::Network Qt6::Multimedia PkgConfig::FFMPEG)
    # Bundled so the app looks the same on every platform; both are OFL licensed.
    qt_add_resources(disquisition-desktop "fonts"
      PREFIX "/fonts"
      BASE resources/fonts
      FILES
        resources/fonts/Inter-Regular.ttf
        resources/fonts/Inter-SemiBold.ttf
        resources/fonts/Inter-Bold.ttf
        resources/fonts/CalSans-Regular.ttf
        resources/fonts/JetBrainsMono-Regular.ttf)
    if(NOT APPLE)
      # The window and tray icon. The macOS bundle has its own.
      qt_add_resources(disquisition-desktop app_icon
        PREFIX /
        BASE resources/macos
        FILES resources/macos/disquisition.png)
    endif()
    if(WIN32)
      target_sources(disquisition-desktop PRIVATE
        include/desktop/windows_notifications.h src/desktop/windows_notifications.cpp)
      target_link_libraries(disquisition-desktop PRIVATE runtimeobject)
    endif()
    if(UNIX AND NOT APPLE AND TARGET Qt6::DBus)
      target_sources(disquisition-desktop PRIVATE
        include/desktop/freedesktop_notifications.h src/desktop/freedesktop_notifications.cpp)
      target_compile_definitions(disquisition-desktop PRIVATE DISQUISITION_DBUS_NOTIFICATIONS)
      target_link_libraries(disquisition-desktop PRIVATE Qt6::DBus)
    endif()
    set(DISQUISITION_GIPHY_API_KEY "" CACHE STRING "Default GIPHY API key for the GIF picker")
    set(DISQUISITION_KLIPY_API_KEY "" CACHE STRING "Default KLIPY API key for the GIF picker")
    target_compile_definitions(disquisition-desktop PRIVATE
      "DISQUISITION_GIPHY_API_KEY=\"${DISQUISITION_GIPHY_API_KEY}\""
      "DISQUISITION_KLIPY_API_KEY=\"${DISQUISITION_KLIPY_API_KEY}\"")
    if(UNIX AND NOT APPLE AND XCB_FOUND)
      target_link_libraries(disquisition-desktop PRIVATE PkgConfig::XCB)
      target_compile_definitions(disquisition-desktop PRIVATE DISQUISITION_HAVE_XCB)
    endif()
    if(WIN32)
      target_link_libraries(disquisition-desktop PRIVATE user32)
    endif()
    if(APPLE)
      set(DISQUISITION_CODESIGN_IDENTITY "AUTO" CACHE STRING
          "macOS signing identity: AUTO prefers Apple Development; '-' uses ad-hoc signing")
      include(cmake/MacOSCodeSigning.cmake)
      disquisition_resolve_codesign_identity("${DISQUISITION_CODESIGN_IDENTITY}"
        DISQUISITION_RESOLVED_CODESIGN_IDENTITY)
      target_sources(desktop_screen PRIVATE
        include/desktop/mac_screen_capture.h src/desktop/mac_screen_capture.mm
        include/desktop/mac_drawing_panel.h src/desktop/mac_drawing_panel.mm)
      set_source_files_properties(src/desktop/audio_permission.cpp PROPERTIES LANGUAGE OBJCXX)
      set_source_files_properties(
        src/desktop/mac_screen_capture.mm src/desktop/mac_drawing_panel.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
      target_sources(disquisition-desktop PRIVATE
        include/desktop/mac_notifications.h src/desktop/mac_notifications.mm)
      set_source_files_properties(src/desktop/mac_notifications.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
      find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
      find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
      find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
      find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
      find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
      find_library(SCREENCAPTUREKIT_FRAMEWORK ScreenCaptureKit REQUIRED)
      find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
      find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
      find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
      target_link_libraries(disquisition-desktop PRIVATE
        ${APPKIT_FRAMEWORK} ${AVFOUNDATION_FRAMEWORK} ${COREAUDIO_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK} ${SCREENCAPTUREKIT_FRAMEWORK} ${COREMEDIA_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK} ${USERNOTIFICATIONS_FRAMEWORK} ${IOKIT_FRAMEWORK})
      target_link_libraries(desktop_screen PRIVATE
        ${APPKIT_FRAMEWORK} ${COREGRAPHICS_FRAMEWORK} ${SCREENCAPTUREKIT_FRAMEWORK} ${COREMEDIA_FRAMEWORK} ${COREVIDEO_FRAMEWORK})
      set_target_properties(disquisition-desktop PROPERTIES
        OUTPUT_NAME "${DISQUISITION_APP_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${DISQUISITION_APP_NAME}"
        MACOSX_BUNDLE_ICON_FILE "disquisition.icns"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${DISQUISITION_BUNDLE_IDENTIFIER}")
      # Packaging reads the actual bundle path, including spaces and the channel.
      file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/disquisition-app-bundle-$<CONFIG>.txt"
        CONTENT "$<TARGET_BUNDLE_DIR:disquisition-desktop>\n")
      target_sources(disquisition-desktop PRIVATE resources/macos/disquisition.icns)
      set_source_files_properties(resources/macos/disquisition.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
      set(DISQUISITION_SIGN_COMMAND "${CMAKE_COMMAND}"
          "-DDISQUISITION_CODESIGN_IDENTITY=${DISQUISITION_RESOLVED_CODESIGN_IDENTITY}"
          "-DDISQUISITION_APP_BUNDLE=$<TARGET_BUNDLE_DIR:disquisition-desktop>"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignMacOS.cmake")
      add_custom_command(TARGET disquisition-desktop POST_BUILD
        COMMAND ${DISQUISITION_SIGN_COMMAND}
        COMMENT "Signing disquisition for macOS permissions"
        VERBATIM)
      # macdeployqt modifies the app after POST_BUILD.
      # Re-sign with the same configured identity once deployment is finished.
      add_custom_target(disquisition-sign-macos
        COMMAND ${DISQUISITION_SIGN_COMMAND}
        DEPENDS disquisition-desktop
        COMMENT "Signing deployed disquisition for macOS permissions"
        VERBATIM)
    endif()
  elseif(NOT Qt6_FOUND)
    message(STATUS "Qt 6.6+ not found; skipping disquisition-desktop")
  else()
    message(STATUS "FFmpeg (libavcodec, libavutil, libswscale) not found; skipping disquisition-desktop")
  endif()
endif()

if(BUILD_TESTING AND TARGET desktop_media)
  add_executable(desktop_media_test test/desktop_media_test.cpp)
  target_link_libraries(desktop_media_test PRIVATE desktop_media)
  add_test(NAME desktop_media_test COMMAND desktop_media_test)
  set_tests_properties(desktop_media_test PROPERTIES TIMEOUT 120)
endif()

if(BUILD_TESTING AND TARGET disquisition-desktop)
  # Exercise the actual widgets and their wiring, including modal dialogs,
  # with isolated identity/settings directories and no audio hardware access.
  get_target_property(COMMUNITY_UI_SOURCES disquisition-desktop SOURCES)
  list(REMOVE_ITEM COMMUNITY_UI_SOURCES src/desktop/main.cpp resources/macos/disquisition.icns)
  get_target_property(COMMUNITY_UI_LIBRARIES disquisition-desktop LINK_LIBRARIES)
  get_target_property(COMMUNITY_UI_DEFINITIONS disquisition-desktop COMPILE_DEFINITIONS)
  add_executable(community_ui_test test/community_ui_test.cpp ${COMMUNITY_UI_SOURCES})
  set_target_properties(community_ui_test PROPERTIES AUTOMOC ON)
  target_link_libraries(community_ui_test PRIVATE ${COMMUNITY_UI_LIBRARIES})
  target_compile_definitions(community_ui_test PRIVATE ${COMMUNITY_UI_DEFINITIONS})
  target_include_directories(community_ui_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  add_test(NAME community_ui_test COMMAND community_ui_test)
  set_tests_properties(community_ui_test PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(BUILD_TESTING AND TARGET desktop_notifications)
  add_executable(desktop_notifications_test test/desktop_notifications_test.cpp)
  target_link_libraries(desktop_notifications_test PRIVATE desktop_notifications)
  add_test(NAME desktop_notifications_test COMMAND desktop_notifications_test)
endif()

if(BUILD_TESTING AND TARGET desktop_screen)
  add_executable(screen_drawing_test test/screen_drawing_test.cpp)
  target_link_libraries(screen_drawing_test PRIVATE desktop_screen)
  add_test(NAME screen_drawing_test COMMAND screen_drawing_test)
  set_tests_properties(screen_drawing_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
endif()

# Run this explicitly in a macOS desktop session; the regular drawing test
# remains headless so CI does not need access to WindowServer.
if(BUILD_TESTING AND APPLE AND TARGET desktop_screen)
  add_executable(mac_drawing_panel_test EXCLUDE_FROM_ALL test/mac_drawing_panel_test.mm)
  target_compile_options(mac_drawing_panel_test PRIVATE -fobjc-arc)
  target_link_libraries(mac_drawing_panel_test PRIVATE desktop_screen ${APPKIT_FRAMEWORK})
endif()

if(BUILD_TESTING AND TARGET desktop_screen AND TARGET chat_communities)
  add_executable(community_drawing_test test/community_drawing_test.cpp)
  target_link_libraries(community_drawing_test PRIVATE desktop_screen chat_communities chat_community_router)
  add_test(NAME community_drawing_test COMMAND community_drawing_test)
  set_tests_properties(community_drawing_test PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
