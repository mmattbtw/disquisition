CXX := g++
# _GNU_SOURCE exposes the POSIX socket declarations on Cygwin/MSYS2.
CXXFLAGS := -std=c++20 -Wall -Wextra -pthread -D_GNU_SOURCE
# spdlog is used header-only here. CMake downloads it on the first build of
# the main project; point SPDLOG_INCLUDE elsewhere to use another copy.
SPDLOG_INCLUDE ?= ../build/_deps/spdlog-src/include
INCLUDES := -I../include -I$(SPDLOG_INCLUDE)

SOURCES := \
	basic_client.cpp \
	../src/client/client.cpp \
	../src/client/connection.cpp \
	../src/client/peer_network.cpp \
	../src/common/net.cpp \
	../src/common/protocol.cpp

.PHONY: all clean

all: basic_client

basic_client: $(SOURCES)
	@test -f $(SPDLOG_INCLUDE)/spdlog/spdlog.h || \
		{ echo "spdlog not found at $(SPDLOG_INCLUDE); run 'make' in the project root first"; exit 1; }
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o basic_client

clean:
	rm -f basic_client
