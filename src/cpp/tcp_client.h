
#pragma once

#include "io_dispatch.h"
#include "fiber.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace tcp_client
{
  struct tcp_caller
  {
    virtual ~tcp_caller() {}
    virtual void exec(bool success, int failure_errno, std::unique_ptr<fiber_pipe>&& pipe) = 0;
  };
  
  template<typename Fn>
  struct tcp_call : public tcp_caller
  {
    tcp_call(Fn f)
      : f_(f)
    {}
  
    virtual void exec(bool success, int failure_errno, std::unique_ptr<fiber_pipe>&& pipe)
    {
      f_(success, failure_errno, std::move(pipe));
    }
    
    Fn f_;
  };
  
  void do_connect_and_run(const char* host, int port, tcp_caller* tcpc, int stack_size);

  // attempt to connect to 'host' / 'port'
  // and when this succeeds or fails call the given
  // 'f' on a newly created fiber.
  //
  // The signature of 'f' must be:
  //
  //    void f(bool success, int failure_errno, std::unique_ptr<fiber_pipe>&& pipe)
  //
  // if host/port can be connected to then f will be
  // be called with 'success' true and a valid 'pipe'.
  // in this case failure_errno will be 0.  If host/port
  // cannot be connected to then 'f' will be called with
  // 'success' false and 'failure_errno' set to the error
  // code for the failure.  In this case pipe is an
  // empty fiber_pipe. 
  template<typename Fn>
  void connect_and_run(const char* host, int port, Fn f, int stack_size=fiber::k_default_stack_size)
  {
    do_connect_and_run(host, port, new tcp_call<Fn>(f), stack_size);
  }  
}

