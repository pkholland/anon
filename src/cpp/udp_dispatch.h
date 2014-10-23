
#pragma once

#include "io_dispatch.h"
#include "src_addr_validator.h"

class udp_dispatch : public io_dispatch::handler
{
public:
  udp_dispatch(int port, const src_addr_validator& validator);
  
  ~udp_dispatch()
  {
    close(sock_);
  }
  
  // messages sent to this machine from an address that is
  // considered 'valid' by the validator_, are passed to this
  // method.
  virtual void recv_msg(const unsigned char* msg, ssize_t len,
                        const struct sockaddr_storage *sockaddr,
                        socklen_t sockaddr_len) = 0;

  virtual void io_avail(io_dispatch& io_d, const struct epoll_event& event);
                        
  void attach(io_dispatch& io_d)
  {
    struct epoll_event evt;
    evt.events = EPOLLIN;
    evt.data.ptr = this;
    io_d.epoll_ctl(EPOLL_CTL_ADD, sock_, &evt);
  }
  
  int get_sock() { return sock_; }
                        
private:  
  
  int sock_;
  const src_addr_validator& validator_;
};


