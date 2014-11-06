/*
 Copyright (c) 2014 Anon authors, see AUTHORS file.
 
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
  
}

