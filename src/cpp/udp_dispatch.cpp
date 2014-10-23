
#include "udp_dispatch.h"
#include <arpa/inet.h>

udp_dispatch::udp_dispatch(int udp_port, const src_addr_validator& validator)
  : validator_(validator)
{
  // no SOCK_CLOEXEC since we inherit this socket down to the child
  // when we do a child swap
  sock_ = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (sock_ == -1)
    do_error("socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0)");

  // bind to any address that will route to this machine
  struct sockaddr_in6 addr = { 0 };
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(udp_port);
  addr.sin6_addr = in6addr_any;
  if (bind(sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0)
  {
    close(sock_);
    do_error("bind(<AF_INET6 SOCK_DGRAM socket>, <" << udp_port << ", in6addr_any>, sizeof(addr))");
  }

  anon_log("listening for udp on port " << udp_port << ", socket " << sock_);
}

void udp_dispatch::io_avail(io_dispatch& io_d, const struct epoll_event& event)
{
  if (event.events & EPOLLIN) {
  
    unsigned char msgBuff[8192];
    while (true) {
      struct sockaddr_storage host;
      socklen_t host_addr_size = sizeof(struct sockaddr_storage);
      auto dlen = recvfrom(sock_, &msgBuff[0], sizeof(msgBuff), 0, (struct sockaddr*)&host, &host_addr_size);
      if (dlen == -1) {
        if (errno != EAGAIN)
          anon_log_error("recvfrom failed with errno: " << errno_string());
        return;
      }
      else if (dlen == sizeof(msgBuff))
        anon_log_error("message too big! all " << sizeof(msgBuff) << " bytes consumed in recvfrom call");
      else if (validator_.is_valid(&host, host_addr_size))
        recv_msg(&msgBuff[0], dlen, &host, host_addr_size);
    }
    
  } else
    anon_log_error("udp_dispatch::io_avail called with no EPOLLIN. event.events = " << event_bits_to_string(event.events));
}


