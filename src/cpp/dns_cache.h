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

#include "fiber.h"
#include "io_dispatch.h"
#include <sys/socket.h>
#include <netinet/in.h>

namespace dns_cache
{

  // any dns entry in the cache that is older than
  // cache_life_seconds will be re-looked up
  const int cache_life_seconds = 120;
  
  void initialize();
  void terminate();

  struct dns_caller
  {
    virtual ~dns_caller() {}
    virtual void exec(int err_code, const struct sockaddr *addr, socklen_t addrlen) = 0;
  };
  
  template<typename Fn>
  struct dns_call : public dns_caller
  {
    dns_call(Fn f)
      : f_(f)
    {}
  
    virtual void exec(int err_code, const struct sockaddr *addr, socklen_t addrlen)
    {
      f_(err_code, addr, addrlen);
    }
    
    Fn f_;
  };
  
  void do_lookup_and_run(const char* host, int port, dns_caller* dnsc, size_t stack_size);

  template<typename Fn>
  void lookup_and_run(const char* host, int port, Fn f, size_t stack_size=fiber::k_default_stack_size)
  {
    do_lookup_and_run(host, port, new dns_call<Fn>(f), stack_size);
  }
  
  // can only be called from within a fiber.
  // if the given host/port is already in the cache and is currently
  // available, then the addr will be filled out immediately and the
  // function returns 0.
  //
  // Otherwise the current fiber is suspended while the dns lookup
  // occurs, or until the eariest allowable time for host to be contacted
  // has passed.  When the dns lookup completes the fiber will be resumed
  // and the function will return 0 if it succeeded in finding an
  // address for host/port, or an error code.  If the error code is
  // >0 it is a gai_strerror error code.  If it is <0 it is a errno code.
  // While the type for the 'addr' parameter is a ipv6 sockaddr, it is
  // frequently the case that dns will return ipv4 addresses.  Look in
  // addr->sin6_family field to tell which one you received (it will be
  // either AF_INET or AF_INET6).
  int get_addrinfo(const char* host, int port, struct sockaddr_in6* addr, socklen_t* addrlen);
  
}

