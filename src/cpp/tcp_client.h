
#pragma once

#include "io_dispatch.h"
#include "fiber.h"
#include "tcp_utils.h"

namespace tcp_client
{
  struct tcp_caller
  {
    virtual ~tcp_caller() {}
    virtual void exec(int err_code, std::unique_ptr<fiber_pipe>&& pipe = std::unique_ptr<fiber_pipe>()) = 0;
  };
  
  template<typename Fn>
  struct tcp_call : public tcp_caller
  {
    tcp_call(Fn f)
      : f_(f)
    {}
  
    virtual void exec(int err_code, std::unique_ptr<fiber_pipe>&& pipe)
    {
      f_(err_code, std::move(pipe));
    }
    
    Fn f_;
  };
  
  void do_connect_and_run(const char* host, int port, tcp_caller* tcpc, int stack_size);

  // attempt to tcp-connect to 'host' / 'port'
  // and when this succeeds or fails call the given
  // 'f' on a newly created fiber.
  //
  // The signature of 'f' must be:
  //
  //    void f(int err_code, std::unique_ptr<fiber_pipe>&& pipe)
  //
  // if host/port can be connected to then f will be
  // be called with 'err_code' = 0 and a valid 'pipe'.
  // In this case 'pipe' is a fiber_pipe wrapped around
  // the tcp socket that is connected to host/port.
  // If host/port cannot be connected to then 'f' will be
  // called with non-zero 'err_code'.  In this case
  // 'pipe' is an empty (null) fiber_pipe.
  //
  // If err_code > 0 then it is a system errno value.
  // If err_code < 0 then it is a "GetAddrInfo" code
  // which can be displayed in human-readable form by
  // calling the system call gai_strerror.
  template<typename Fn>
  void connect_and_run(const char* host, int port, Fn f, int stack_size=fiber::k_default_stack_size)
  {
    do_connect_and_run(host, port, new tcp_call<Fn>(f), stack_size);
  }  
}

