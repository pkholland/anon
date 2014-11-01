
#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


// stream out a sockaddr in a human-readable form
template<typename T>
T& operator<<(T& str, const struct sockaddr_storage& addr)
{
  char  ipaddr[64] = { 0 };
  int   port = 0;
  if (addr.ss_family == AF_INET6) {
    inet_ntop(AF_INET6, &((struct ::sockaddr_in6*)&addr)->sin6_addr, ipaddr, sizeof(ipaddr));
    port = ntohs(((struct ::sockaddr_in6*)&addr)->sin6_port);
  } else if (addr.ss_family == AF_INET) {
    inet_ntop(AF_INET, &((struct ::sockaddr_in*)&addr)->sin_addr, ipaddr, sizeof(ipaddr));
    port = ntohs(((struct ::sockaddr_in*)&addr)->sin_port);
  }
  return str << ipaddr << "/" << port;
}

