cmake_minimum_required(VERSION 3.16)
project(disquisition LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

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

add_library(chat_protocol STATIC src/common/protocol.cpp)
target_include_directories(chat_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(server
  src/server/main.cpp
  src/server/server.cpp
  src/server/database.cpp)
target_link_libraries(server PRIVATE chat_protocol ${SQLITE_TARGET} Threads::Threads)

add_executable(client
  src/client/main.cpp
  src/client/tui.cpp
  src/client/connection.cpp
  src/client/peer_network.cpp)
target_include_directories(client PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(client PRIVATE chat_protocol ${CURSES_LIBRARIES} Threads::Threads)

add_executable(relay
  src/relay/main.cpp
  src/relay/relay.cpp
  src/client/connection.cpp
  src/client/peer_network.cpp)
target_link_libraries(relay PRIVATE chat_protocol Threads::Threads)
