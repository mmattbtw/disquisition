cmake_minimum_required(VERSION 3.16)
project(disquisition LANGUAGES CXX)

option(BUILD_DESKTOP_APP "Build the Qt desktop chat and voice app" ON)
include(cmake/AppIdentity.cmake)
if(WIN32)
  option(BUILD_LEGACY_TARGETS "Build the server, relay and C++ client library" OFF)
else()
  option(BUILD_LEGACY_TARGETS "Build the server, relay and C++ client library" ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.3
)
FetchContent_MakeAvailable(spdlog)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()
# Exposes the POSIX socket and signal declarations on platforms such as
# Cygwin/MSYS2 whose strict C++ mode would otherwise hide them.
add_compile_definitions(_GNU_SOURCE)

# Homebrew's SQLite is keg-only, so CMake can otherwise pick the older macOS SDK
# copy. Use installed Homebrew dependencies when present, including on Intel Macs.
if(APPLE)
  find_program(HOMEBREW_EXECUTABLE brew)
  if(HOMEBREW_EXECUTABLE)
    execute_process(
      COMMAND "${HOMEBREW_EXECUTABLE}" --prefix sqlite
      OUTPUT_VARIABLE HOMEBREW_SQLITE_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
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

# CMake renamed this imported target at some point; support both spellings.
if(TARGET SQLite3::SQLite3)
  set(SQLITE_TARGET SQLite3::SQLite3)
else()
  set(SQLITE_TARGET SQLite::SQLite3)
endif()

if(BUILD_LEGACY_TARGETS)
  find_package(Threads REQUIRED)
endif()

# Wire protocol and socket helpers shared by every program.
add_library(chat_protocol STATIC
  src/common/protocol.cpp
  src/common/command_line.cpp
  src/util/log.cpp)
# The socket helpers use POSIX sockets. Only the legacy targets call them, so
# a Windows build of the desktop app leaves them out.
if(BUILD_LEGACY_TARGETS)
  target_sources(chat_protocol PRIVATE src/common/net.cpp)
endif()
target_include_directories(chat_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(chat_protocol PUBLIC spdlog::spdlog)

add_library(chat_recent_messages STATIC src/common/recent_messages.cpp)
target_include_directories(chat_recent_messages PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(chat_recent_messages PRIVATE ${SQLITE_TARGET})

if(BUILD_DESKTOP_APP)
  if(APPLE)
    enable_language(OBJCXX)
  endif()
  # Linux desktop notifications go over D-Bus when Qt has it.
  find_package(Qt6 6.6 COMPONENTS Widgets Network Multimedia OPTIONAL_COMPONENTS DBus QUIET)
  # Screen sharing encodes and decodes H.264 video and Opus sound with FFmpeg.
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
      libavcodec>=60 libavutil libswresample libswscale)
  endif()
  if(Qt6_FOUND AND FFMPEG_FOUND)
    add_library(desktop_penguins STATIC
      src/desktop/chat_color.cpp src/desktop/penguin_overlay.cpp
      src/desktop/penguin_sprites.cpp)
    target_include_directories(desktop_penguins PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(desktop_penguins PUBLIC Qt6::Widgets)
    qt_add_resources(desktop_penguins penguins
      PREFIX "/penguins"
      BASE resources/penguins
      FILES
        resources/penguins/walk/1.png resources/penguins/walk/2.png
        resources/penguins/walk/3.png resources/penguins/walk/4.png
        resources/penguins/walk/5.png resources/penguins/walk/6.png
        resources/penguins/walk/7.png resources/penguins/walk/8.png)
    if(APPLE)
      target_sources(desktop_penguins PRIVATE src/desktop/mac_penguin_overlay.mm)
      find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
      target_link_libraries(desktop_penguins PRIVATE ${APPKIT_FRAMEWORK})
    elseif(WIN32)
      target_link_libraries(desktop_penguins PRIVATE user32)
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
    target_link_libraries(desktop_media PUBLIC chat_protocol Qt6::Gui Qt6::Network)

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
    target_link_libraries(desktop_screen PUBLIC chat_protocol Qt6::Widgets Qt6::Multimedia)
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
    # Opus sound for screen shares, and the device helpers it shares with voice.
    add_library(desktop_audio STATIC
      include/desktop/audio_device.h
      include/desktop/screen_audio.h
      src/desktop/audio_device.cpp
      src/desktop/screen_audio.cpp)
    set_target_properties(desktop_audio PROPERTIES AUTOMOC ON)
    target_include_directories(desktop_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(desktop_audio PUBLIC Qt6::Multimedia PRIVATE PkgConfig::FFMPEG)

    add_executable(disquisition-desktop MACOSX_BUNDLE
      include/desktop/audio_permission.h
      include/desktop/chat_media.h
      include/desktop/desktop_notifier.h
      include/desktop/gif_picker.h
      include/desktop/main_window.h
      include/desktop/push_to_talk.h
      include/desktop/mention_completer.h
      include/desktop/notification_center.h
      include/desktop/relay_audio.h
      include/desktop/screen_capture.h
      include/desktop/screen_h264.h
      include/desktop/share_picker.h
      include/desktop/system_audio_capture.h
      include/desktop/voice_engine.h
      src/desktop/audio_permission.cpp
      src/desktop/chat_media.cpp
      src/desktop/desktop_notifier.cpp
      src/desktop/gif_picker.cpp
      src/desktop/main.cpp
      src/desktop/main_window.cpp
      src/desktop/push_to_talk.cpp
      src/desktop/mention_completer.cpp
      src/desktop/notification_center.cpp
      src/desktop/relay_audio.cpp
      src/desktop/screen_capture.cpp
      src/desktop/screen_h264.cpp
      src/desktop/share_picker.cpp
      src/desktop/system_audio_capture.cpp
      src/desktop/voice_engine.cpp)
    set_target_properties(disquisition-desktop PROPERTIES AUTOMOC ON)
    configure_file(cmake/app_identity.h.in generated/desktop/app_identity.h @ONLY)
    target_include_directories(disquisition-desktop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(disquisition-desktop PRIVATE
      chat_protocol chat_recent_messages desktop_audio desktop_media desktop_penguins
      desktop_notifications desktop_screen
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
    # Linux shares sound through PulseAudio, or PipeWire's PulseAudio server.
    if(UNIX AND NOT APPLE)
      pkg_check_modules(PULSE QUIET IMPORTED_TARGET libpulse)
      if(PULSE_FOUND)
        target_link_libraries(disquisition-desktop PRIVATE PkgConfig::PULSE)
        target_compile_definitions(disquisition-desktop PRIVATE DISQUISITION_HAVE_PULSE)
      else()
        message(STATUS "libpulse not found; Linux screen shares will have no sound")
      endif()
    endif()
    if(WIN32)
      # Shared sound is captured with WASAPI process loopback.
      target_link_libraries(disquisition-desktop PRIVATE user32 ole32 mmdevapi)
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
      find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
      find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
      find_library(SCREENCAPTUREKIT_FRAMEWORK ScreenCaptureKit REQUIRED)
      find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
      find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
      find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
      target_link_libraries(disquisition-desktop PRIVATE
        ${APPKIT_FRAMEWORK} ${AVFOUNDATION_FRAMEWORK} ${COREAUDIO_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK} ${IOKIT_FRAMEWORK} ${SCREENCAPTUREKIT_FRAMEWORK} ${COREMEDIA_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK} ${USERNOTIFICATIONS_FRAMEWORK})
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
      # macdeployqt and bundling baresip modify the app after POST_BUILD.
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
    message(STATUS "FFmpeg (libavcodec, libavutil, libswresample, libswscale) not found; skipping disquisition-desktop")
  endif()
endif()

if(BUILD_LEGACY_TARGETS)
# Client-side networking: server/relay connections and the peer mesh. The
# relay reuses the mesh to host its users.
add_library(chat_network STATIC
  src/client/connection.cpp
  src/client/peer_network.cpp)
target_link_libraries(chat_network PUBLIC chat_protocol Threads::Threads)

# The public client library. Its header exposes Connection and PeerNetwork,
# so chat_network is part of its interface.
add_library(disquisition_client STATIC src/client/client.cpp)
add_library(disquisition::client ALIAS disquisition_client)
set_target_properties(disquisition_client PROPERTIES OUTPUT_NAME disquisition-client)
target_link_libraries(disquisition_client PUBLIC chat_network)

add_executable(server
  src/server/main.cpp
  src/server/options.cpp
  src/server/server.cpp
  src/server/database.cpp)
target_link_libraries(server PRIVATE chat_protocol ${SQLITE_TARGET})

add_executable(relay
  src/relay/main.cpp
  src/relay/options.cpp
  src/relay/relay.cpp)
target_link_libraries(relay PRIVATE chat_network)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME app_identity_test
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/app_identity_test.cmake")
  add_test(NAME macos_codesign_test
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/macos_codesign_test.cmake")
  if(UNIX)
    add_test(NAME hdiutil_retry_test
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test/hdiutil_retry_test.sh")
  endif()
  add_test(NAME macos_dmg_retry_test
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test/macos_dmg_retry_test.sh")
  add_executable(protocol_test test/protocol_test.cpp)
  target_link_libraries(protocol_test PRIVATE chat_protocol)
  add_test(NAME protocol_test COMMAND protocol_test)

  add_executable(recent_messages_test test/recent_messages_test.cpp)
  target_link_libraries(recent_messages_test PRIVATE chat_recent_messages ${SQLITE_TARGET})
  add_test(NAME recent_messages_test COMMAND recent_messages_test)

  add_executable(connection_test test/connection_test.cpp)
  target_link_libraries(connection_test PRIVATE chat_network)
  add_test(NAME connection_test COMMAND connection_test)

  add_executable(client_library_test test/client_library_test.cpp)
  target_link_libraries(client_library_test PRIVATE disquisition_client)
  add_test(NAME client_library_test COMMAND client_library_test)

  # Runs the real server and relay executables as child processes.
  add_executable(end_to_end_test test/end_to_end_test.cpp)
  target_link_libraries(end_to_end_test PRIVATE disquisition_client)
  add_test(NAME end_to_end_test
    COMMAND end_to_end_test $<TARGET_FILE:server> $<TARGET_FILE:relay> "${CMAKE_CURRENT_BINARY_DIR}")

  set_tests_properties(protocol_test connection_test client_library_test end_to_end_test PROPERTIES TIMEOUT 120)
endif()
endif()

if(BUILD_TESTING AND TARGET desktop_media)
  add_executable(desktop_media_test test/desktop_media_test.cpp)
  target_link_libraries(desktop_media_test PRIVATE desktop_media)
  add_test(NAME desktop_media_test COMMAND desktop_media_test)
  set_tests_properties(desktop_media_test PROPERTIES TIMEOUT 120)
endif()

if(BUILD_TESTING AND TARGET desktop_audio)
  add_executable(screen_audio_test test/screen_audio_test.cpp)
  target_link_libraries(screen_audio_test PRIVATE desktop_audio)
  add_test(NAME screen_audio_test COMMAND screen_audio_test)
endif()

if(BUILD_TESTING AND TARGET desktop_penguins)
  add_executable(penguin_overlay_test test/penguin_overlay_test.cpp)
  target_link_libraries(penguin_overlay_test PRIVATE desktop_penguins)
  if(APPLE)
    set_source_files_properties(test/penguin_overlay_test.cpp PROPERTIES LANGUAGE OBJCXX)
    target_link_libraries(penguin_overlay_test PRIVATE ${APPKIT_FRAMEWORK})
  endif()
  add_test(NAME penguin_overlay_test COMMAND penguin_overlay_test)
  set_tests_properties(penguin_overlay_test PROPERTIES
    TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
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

if(BUILD_TESTING AND TARGET desktop_screen AND TARGET chat_network)
  add_executable(screen_drawing_network_test test/screen_drawing_network_test.cpp)
  target_link_libraries(screen_drawing_network_test PRIVATE desktop_screen chat_network)
  add_test(NAME screen_drawing_network_test
    COMMAND screen_drawing_network_test $<TARGET_FILE:server> $<TARGET_FILE:relay>)
  set_tests_properties(screen_drawing_network_test PROPERTIES TIMEOUT 60)
endif()
