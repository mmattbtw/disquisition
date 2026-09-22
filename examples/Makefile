CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pthread
INCLUDES := -I../include -I../src

SOURCES := \
	basic_client.cpp \
	../src/lib/client.cpp \
	../src/client/connection.cpp \
	../src/client/peer_network.cpp \
	../src/common/protocol.cpp

.PHONY: all clean

all: basic_client

basic_client: $(SOURCES)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o basic_client

clean:
	rm -f basic_client
