cmake_minimum_required(VERSION 3.16)
project(disquisition LANGUAGES CXX)

option(BUILD_DESKTOP_APP "Build the Qt desktop chat and voice app" ON)
if(WIN32)
  option(BUILD_LEGACY_TARGETS "Build the server, relay, terminal client and library" OFF)
else()
  option(BUILD_LEGACY_TARGETS "Build the server, relay, terminal client and library" ON)
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

add_compile_options(-Wall -Wextra -Wpedantic)
# Exposes the POSIX socket and signal declarations on platforms such as
# Cygwin/MSYS2 whose strict C++ mode would otherwise hide them.
add_compile_definitions(_GNU_SOURCE)

if(BUILD_LEGACY_TARGETS)
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

    execute_process(
      COMMAND "${HOMEBREW_EXECUTABLE}" --prefix ncurses
      OUTPUT_VARIABLE HOMEBREW_NCURSES_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if((NOT DEFINED CURSES_NCURSES_LIBRARY OR
        CURSES_NCURSES_LIBRARY MATCHES "MacOSX[.]sdk|CommandLineTools") AND
       EXISTS "${HOMEBREW_NCURSES_PREFIX}/include/ncurses.h" AND
       EXISTS "${HOMEBREW_NCURSES_PREFIX}/lib/libncurses.dylib")
      set(CURSES_NCURSES_LIBRARY "${HOMEBREW_NCURSES_PREFIX}/lib/libncurses.dylib" CACHE FILEPATH "ncurses library" FORCE)
      set(CURSES_INCLUDE_PATH "${HOMEBREW_NCURSES_PREFIX}/include" CACHE PATH "ncurses headers" FORCE)
    endif()
  endif()
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 3.24 REQUIRED)

# CMake renamed this imported target at some point; support both spellings.
if(TARGET SQLite3::SQLite3)
  set(SQLITE_TARGET SQLite3::SQLite3)
else()
  set(SQLITE_TARGET SQLite::SQLite3)
endif()

set(CURSES_NEED_NCURSES TRUE)
find_package(Curses REQUIRED)
endif()

# Wire protocol and socket helpers shared by every program.
add_library(chat_protocol STATIC
  src/common/protocol.cpp
  src/common/command_line.cpp
  src/common/net.cpp
  src/util/log.cpp)
target_include_directories(chat_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(chat_protocol PUBLIC spdlog::spdlog)

if(BUILD_DESKTOP_APP)
  if(APPLE)
    enable_language(OBJCXX)
  endif()
  find_package(Qt6 6.5 COMPONENTS Widgets Network Multimedia QUIET)
  if(Qt6_FOUND)
    add_executable(disquisition-desktop MACOSX_BUNDLE
      src/desktop/audio_permission.h
      src/desktop/audio_permission.cpp
      src/desktop/main.cpp
      src/desktop/main_window.cpp
      src/desktop/main_window.h
      src/desktop/relay_audio.cpp
      src/desktop/relay_audio.h
      src/desktop/voice_engine.cpp
      src/desktop/voice_engine.h)
    set_target_properties(disquisition-desktop PROPERTIES AUTOMOC ON)
    target_include_directories(disquisition-desktop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(disquisition-desktop PRIVATE
      chat_protocol Qt6::Widgets Qt6::Network Qt6::Multimedia)
    if(APPLE)
      set_source_files_properties(src/desktop/audio_permission.cpp PROPERTIES LANGUAGE OBJCXX)
      find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
      find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
      target_link_libraries(disquisition-desktop PRIVATE
        ${AVFOUNDATION_FRAMEWORK} ${COREAUDIO_FRAMEWORK})
      set_target_properties(disquisition-desktop PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Disquisition"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.disquisition.desktop")
      add_custom_command(TARGET disquisition-desktop POST_BUILD
        COMMAND codesign --force --deep --sign -
          "$<TARGET_BUNDLE_DIR:disquisition-desktop>"
        COMMENT "Ad-hoc signing Disquisition for macOS permissions")
    endif()
  else()
    message(STATUS "Qt 6.5+ not found; skipping disquisition-desktop")
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

add_executable(client
  src/client/main.cpp
  src/client/options.cpp
  src/client/text_wrap.cpp
  src/client/tui.cpp)
target_include_directories(client PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(client PRIVATE chat_network ${CURSES_LIBRARIES})

add_executable(relay
  src/relay/main.cpp
  src/relay/options.cpp
  src/relay/relay.cpp)
target_link_libraries(relay PRIVATE chat_network)

include(CTest)
if(BUILD_TESTING)
  add_executable(protocol_test test/protocol_test.cpp)
  target_link_libraries(protocol_test PRIVATE chat_protocol)
  add_test(NAME protocol_test COMMAND protocol_test)

  add_executable(connection_test test/connection_test.cpp)
  target_link_libraries(connection_test PRIVATE chat_network)
  add_test(NAME connection_test COMMAND connection_test)

  add_executable(text_wrap_test test/text_wrap_test.cpp src/client/text_wrap.cpp)
  target_include_directories(text_wrap_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME text_wrap_test COMMAND text_wrap_test)

  add_executable(client_library_test test/client_library_test.cpp)
  target_link_libraries(client_library_test PRIVATE disquisition_client)
  add_test(NAME client_library_test COMMAND client_library_test)

  # Runs the real server and relay executables as child processes.
  add_executable(end_to_end_test test/end_to_end_test.cpp)
  target_link_libraries(end_to_end_test PRIVATE disquisition_client)
  add_test(NAME end_to_end_test
    COMMAND end_to_end_test $<TARGET_FILE:server> $<TARGET_FILE:relay> "${CMAKE_CURRENT_BINARY_DIR}")

  set_tests_properties(protocol_test text_wrap_test connection_test client_library_test end_to_end_test PROPERTIES TIMEOUT 120)
endif()
endif()
