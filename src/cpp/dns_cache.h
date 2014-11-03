
#pragma once

#include "fiber.h"
#include "io_dispatch.h"
#include <sys/socket.h>

namespace dns_cache
{

  // any dns entry in the cache that is older than
  // cache_life_seconds will be re-looked up
  const int cache_life_seconds = 120;
  
  void attach(io_dispatch& io_d);

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

