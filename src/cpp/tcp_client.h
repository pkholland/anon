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

#include "io_dispatch.h"
#include "fiber.h"
#include "tcp_utils.h"

namespace tcp_client
{
struct tcp_caller
{
  virtual ~tcp_caller() {}
  virtual void exec(int err_code, std::unique_ptr<fiber_pipe> &&pipe = std::unique_ptr<fiber_pipe>()) = 0;
};

template <typename Fn>
struct tcp_call : public tcp_caller
{
  tcp_call(Fn f)
      : f_(f)
  {
  }

  virtual void exec(int err_code, std::unique_ptr<fiber_pipe> &&pipe)
  {
    try
    {
      f_(err_code, std::move(pipe));
    }
    catch (const std::runtime_error &ex)
    {
// "runtime" errors are common when a client closes
// their side of a socket without telling us first.
// So we log that only at pretty high levels of logging
#if ANON_LOG_NET_TRAFFIC > 1
      anon_log("uncaught exception in tcp, what() = " << ex.what());
#endif
    }
  }

  Fn f_;
};

void do_connect_and_run(const char *host, int port, tcp_caller *tcpc, size_t stack_size, bool non_blocking);

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
template <typename Fn>
void connect_and_run(const char *host, int port, Fn f, size_t stack_size = fiber::k_default_stack_size, bool non_blocking = true)
{
  do_connect_and_run(host, port, new tcp_call<Fn>(f), stack_size, non_blocking);
}

// can only be called from a fiber.  The calling fiber will
// be suspended while the dns (cache) lookup and tcp connect
// are performed.  The return value is a pair where the first
// element is either 0 (the function succeeded and the second
// element is valid) or is an error code (function failed and
// second element is invalid).  These are the same parameters
// that are passed to 'f' in connect_and_run, described above.
std::pair<int, std::unique_ptr<fiber_pipe>> connect(const char *host, int port);

// similar to the version of connect above, except that no dns
// logic is involved
std::pair<int, std::unique_ptr<fiber_pipe>> connect(const struct sockaddr *addr, socklen_t addrlen, bool non_blocking = true);
} // namespace tcp_client
