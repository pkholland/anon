
#include "udp_dispatch2.h"

udp_dispatch::udp_dispatch(int udp_port, const src_addr_validator& validator)
  : validator_(validator)
{
  // no SOCK_CLOEXEC since we inherit this socket down to the child
  // when we do a child swap
  sock_ = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (sock_ == -1)
    do_error("socket");

  // bind to any address that will route to this machine
  struct sockaddr_in6 addr = { 0 };
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(udp_port);
  addr.sin6_addr = in6addr_any;
  if (bind(udp_sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0)
  {
    close(udp_sock_);
    do_error("bind");
  }

  anon_log("listening for udp on port " << udp_port << ", socket " << udp_sock_);
}

void udp_dispatch::io_avail2(const io_dispatch& io_d, const struct epoll_event& event, bool first_time)
{
  if (event.events & EPOLLIN) {
  
    unsigned char msgBuff[8192];
    while (true) {
      struct sockaddr_storage host;
      socklen_t host_addr_size = sizeof(struct sockaddr_storage);
      auto dlen = recvfrom(sock_, &msgBuff[0], sizeof(msgBuff), 0, (struct sockaddr*)&host, &host_addr_size);
      if (dlen == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (first_time) {
            struct epoll_event evt;
            evt.events = EPOLLET | EPOLLIN;
            evt.data.ptr = this;
            if (io_d->epoll_ctl(EPOLL_CTL_ADD, sock_, &evt) < 0)
              do_error("epoll_ctl");
          }
        }
        else
          anon_log("recvfrom failed with errno: " << errno_string());
        return;
      }
      else if (dlen == sizeof(msgBuff))
        anon_log("message too big!");
      else if (validator_.is_valid(&host, host_addr_size);
        recv_msg(&msgBuff[0], dlen, &host, host_addr_size);
    }
    
  }
}


