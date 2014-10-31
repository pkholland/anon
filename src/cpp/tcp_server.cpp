
#include "tcp_server.h"

void tcp_server::init_socket(int tcp_port, int listen_backlog)
{
  // no SOCK_CLOEXEC since we inherit this socket down to the child
  // when we do a child swap
  listen_sock_ = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (listen_sock_ == -1)
    do_error("socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP)");

  // bind to any address that will route to this machine
  struct sockaddr_in6 addr = { 0 };
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(tcp_port);
  addr.sin6_addr = in6addr_any;
  if (bind(listen_sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0)
  {
    close(listen_sock_);
    do_error("bind(<AF_INET6 SOCK_STREAM socket>, <" << tcp_port << ", in6addr_any>, sizeof(addr))");
  }
  
  if (listen(listen_sock_, listen_backlog) != 0)
  {
    close(listen_sock_);
    do_error("listen(sock_, " << listen_backlog << ")");
  }

  anon_log("listening for tcp connections on port " << tcp_port << ", socket " << listen_sock_);
}

void tcp_server::io_avail(io_dispatch& io_d, const struct epoll_event& event)
{
  if (event.events & EPOLLIN) {
  
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int conn = accept4(listen_sock_,(struct sockaddr*)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn == -1) {
      // we can get EAGAIN because multiple id_d threads
      // can wake up from a single EPOLLIN event.
      // don't bother reporting those.
      if (errno != EAGAIN)
        anon_log_error("accept4(sock_,(struct sockaddr*)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC)");
    } else {
      if (validator_.is_valid(&addr, addr_len)) {
        anon_log("new tcp connection from addr: " << addr);
        fiber::run_in_fiber([conn,addr,addr_len,this]{new_conn_->exec(conn,(struct sockaddr*)&addr,addr_len);});
      } else {
        anon_log("blocked connection attempt from addr: " << addr);
        close(conn);
      }
    }
    
  } else
    anon_log_error("tcp_server::io_avail called with no EPOLLIN. event.events = " << event_bits_to_string(event.events));
}

