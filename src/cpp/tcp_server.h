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

#include "io_dispatch.h"
#include "src_addr_validator.h"
#include "fiber.h"
#include "tcp_utils.h"

class tcp_server : public io_dispatch::handler
{
public:

  // this is the default 'backlog' value to listen(2)
  // and is significantly smaller than SOMAXCONN
  // (which is usually 128).
  enum {
    k_default_backlog = 32
  };

  // whenever a new tcp connection is established
  // on this machine at port 'tcp_port', from a
  // source addr that is considered valid by
  // 'validator', the given 'f' will be executed in
  // a newly constructed fiber.
  //
  // the signature of 'f' needs to be:
  //
  //    void f(std::unique_ptr<fiber_pipe>&&, const sockaddr* src_addr, socklen_t src_addr_len)
  //
  // the 'sock' parameter will be the newly created socket, and it
  // will have both NONBLOCK and CLOEXEC attributes set on it.
  // the src_addr and src_addr_len will be the ip address of the
  // machine that initiated the connection.  This address will be
  // in an ipv6 format, so if the client was, in fact, using an ipv4
  // address it will lock like a 'tunneled' address
  template<typename Fn>
  tcp_server(int tcp_port, const src_addr_validator& validator, Fn f, int listen_backlog = k_default_backlog)
    : new_conn_(new new_con<Fn>(f)),
      validator_(validator)
  {
    init_socket(tcp_port, listen_backlog);
  }
  
  ~tcp_server()
  {
    close(listen_sock_);
  }

  virtual void io_avail(io_dispatch& io_d, const struct epoll_event& event);
                        
  void attach(io_dispatch& io_d)
  {
    io_d.epoll_ctl(EPOLL_CTL_ADD, listen_sock_, EPOLLIN, this);
  }
                        
private:

  void init_socket(int tcp_port, int backlog);

  struct new_connection
  {
    virtual ~new_connection() {}
    virtual void exec(int sock, const sockaddr* src_addr, socklen_t src_addr_len) = 0;
  };

  template<typename Fn>
  struct new_con : public new_connection
  {
    new_con(Fn f)
      : f_(f)
    {}
    
    virtual void exec(int sock, const sockaddr* src_addr, socklen_t src_addr_len)
    {
      f_(std::unique_ptr<fiber_pipe>(new fiber_pipe(sock,fiber_pipe::network)), src_addr, src_addr_len);
    }
    
    Fn f_;
  };
  
  std::unique_ptr<new_connection> new_conn_;
  int listen_sock_;
  const src_addr_validator& validator_;
};


