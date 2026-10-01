#pragma once

#include <sys/socket.h>

// Only override the two calls that expose Horizon's unusual connection
// status. The rest of Transport uses real nonblocking loopback sockets.
int handheld_test_getsockopt(int, int, int, void*, socklen_t*);
int handheld_test_getpeername(int, sockaddr*, socklen_t*);

#define getsockopt handheld_test_getsockopt
#define getpeername handheld_test_getpeername
