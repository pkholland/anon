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
#include <netinet/in.h>

namespace dns_lookup
{
  struct dns_caller
  {
    virtual ~dns_caller() {}
    virtual void exec(int err_code, const std::vector<sockaddr_in6>& addrs) = 0;
  };
  
  template<typename Fn>
  struct dns_call : public dns_caller
  {
    dns_call(Fn f)
      : f_(f)
    {}
  
    virtual void exec(int err_code, const std::vector<sockaddr_in6>& addrs)
    {
      f_(err_code, addrs);
    }
    
    Fn f_;
  };
  
  void do_lookup_and_run(const char* host, int port, dns_caller* dnsc, size_t stack_size);

  // perform an async dns lookup of host/port, and when
  // complete execute f(err_code, addrs) in a newly created
  // fiber using the given stack_size
  template<typename Fn>
  void lookup_and_run(const char* host, int port, Fn f, size_t stack_size=fiber::k_default_stack_size)
  {
    do_lookup_and_run(host, port, new dns_call<Fn>(f), stack_size);
  }
  
  // stall this fiber and initiate an async dns lookup,
  // once that dns lookup completes resume this fiber
  // and return the looked up information.  .first is the
  // err_code.  If this is zero then .second is one or
  // more sockaddrs returned by the dns lookup.  They are
  // stored as inet6 addrs, but can be either normal inet(4)
  // or inet6.  You can tell by looking at sa6_family field.
  // If .first != 0, then an error has occured and .second
  // is empty.  Error's less than 0 indicate gai errors.
  // errors greater than 0 indicate errno errors.
  std::pair<int, std::vector<sockaddr_in6>> get_addrinfo(const char* host, int port);
}

