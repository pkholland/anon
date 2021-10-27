/*
 Copyright (c) 2015 Anon authors, see AUTHORS file.
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

// stream out a sockaddr in a human-readable form
template <typename T>
T &operator<<(T &str, const struct sockaddr_storage &addr)
{
  char ipaddr[64] = {0};
  int port = 0;
  if (addr.ss_family == AF_INET6)
  {
    inet_ntop(AF_INET6, &((struct ::sockaddr_in6 *)&addr)->sin6_addr, ipaddr, sizeof(ipaddr));
    port = ntohs(((struct ::sockaddr_in6 *)&addr)->sin6_port);
  }
  else if (addr.ss_family == AF_INET)
  {
    inet_ntop(AF_INET, &((struct ::sockaddr_in *)&addr)->sin_addr, ipaddr, sizeof(ipaddr));
    port = ntohs(((struct ::sockaddr_in *)&addr)->sin_port);
  }
  return str << ipaddr << "/" << port;
}

template <typename T>
T &operator<<(T &str, const struct sockaddr &addr)
{
  return str << *(const struct sockaddr_storage *)&addr;
}

template <typename T>
T &operator<<(T &str, const struct sockaddr_in6 &addr)
{
  return str << *(const struct sockaddr_storage *)&addr;
}

inline bool operator==(const struct sockaddr_in6 &addr1, const struct sockaddr_in6 &addr2)
{
  if (addr1.sin6_family != addr2.sin6_family)
    return false;
  size_t sz = addr1.sin6_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
  return memcmp(&addr1, &addr2, sz) == 0;
}

inline bool operator<(const struct sockaddr_in6 &addr1, const struct sockaddr_in6 &addr2)
{
  if (addr1.sin6_family != addr2.sin6_family)
    return addr1.sin6_family < addr2.sin6_family;
  size_t sz = addr1.sin6_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
  return memcmp(&addr1, &addr2, sz) < 0;
}

inline bool operator<(const struct sockaddr_storage &addr1, const struct sockaddr_storage &addr2)
{
  if (addr1.ss_family != addr2.ss_family)
    return addr1.ss_family < addr2.ss_family;
  size_t sz = addr1.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
  return memcmp(&addr1, &addr2, sz) < 0;
}
