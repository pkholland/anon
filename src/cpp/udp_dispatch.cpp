
#include "udp_dispatch.h"
#include "log.h"
#include <sys/epoll.h>
#include <system_error>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <string.h>

void udp_dispatch::test_msg()
{
  struct sockaddr_in6 addr = { 0 };
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(udp_port_);
  addr.sin6_addr = in6addr_loopback;
    
  const char* buf = "hello world";
  for (int i = 0; i < 20; i++)
    if (sendto(udp_sock_, buf, strlen(buf) + 1, 0, (struct sockaddr *)&addr, sizeof(addr)) == -1)
      anon_log_error("sendto failed with errno: " << errno_string());
}


int                                     udp_dispatch::ep_fd_;
int                                     udp_dispatch::udp_sock_;
int                                     udp_dispatch::udp_port_;
udp_dispatch::message_handler_function  udp_dispatch::message_handler_;
std::vector<std::thread>                udp_dispatch::io_threads_;
bool                                    udp_dispatch::running_;

namespace {

inline void do_error(const char* fn)
{
  anon_log_error(fn << " failed with errno: " << errno_string());
  throw std::system_error(errno, std::system_category());
}

inline void do_error(const char* fn, int retCode)
{
  anon_log_error(fn << " failed with return value = " << retCode);
  throw std::runtime_error(fn);
}

}

void udp_dispatch::init(int num_threads, int udp_port,
                        message_handler_function message_handler,
                        bool do_inline)
{
  message_handler_ = message_handler;
  udp_port_ = udp_port;

  ep_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (ep_fd_ < 0)
    do_error("epoll_create1");

  anon_log("using fd " << ep_fd_ << " for epoll");

  running_ = true;
  init_udp_socket();

  if (do_inline)
    --num_threads;
  for (int i = 0; i < num_threads; i++)
    io_threads_.push_back(std::thread(&udp_dispatch::read_loop));
  if (do_inline)
    read_loop();
}

void udp_dispatch::init_udp_socket()
{
  // create a udp socket that is non-blocking and has the cloexec bit set
  udp_sock_ = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (udp_sock_ == -1)
    do_error("socket");

  // bind to any address that will route to this machine
  struct sockaddr_in6 addr = { 0 };
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(udp_port_);
  addr.sin6_addr = in6addr_any;
  if (bind(udp_sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    do_error("bind");

  anon_log("listening for udp on port " << udp_port_ << ", socket " << udp_sock_);

  // dispatch any messsages that happen to already be available
  // on the udp socket and then get the socket in the right state
  // for epoll_wait
  udp_dispatch::recv_msg(true/*firsttime*/);
}

// attempt to read the next message from udp_sock_.
// if it gets a message it calls message_handler_ with it
void udp_dispatch::recv_msg(bool firsttime)
{
  unsigned char msgBuff[8192];

  while (running_) {
    struct sockaddr_storage host;
    socklen_t host_addr_size = sizeof(struct sockaddr_storage);
    auto dlen = recvfrom(udp_sock_, &msgBuff[0], sizeof(msgBuff), 0, (struct sockaddr*)&host, &host_addr_size);
    if (dlen == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (firsttime) {
          struct epoll_event evt;
          evt.events = EPOLLET | EPOLLIN;
          evt.data.fd = udp_sock_;
          if (epoll_ctl(ep_fd_, EPOLL_CTL_ADD, udp_sock_, &evt) < 0)
            do_error("epoll_ctl");
        }
      }
      else
        anon_log("recvfrom failed with errno: " << errno_string());
      return;
    }
    else if (dlen == sizeof(msgBuff))
      anon_log("message too big!");
    else
      (*message_handler_)(&msgBuff[0], dlen, &host, host_addr_size);
  }

}

static std::string event_bits_to_string(uint32_t event_bits)
{
    std::string eventstr;
    if (event_bits & EPOLLPRI)
        eventstr += "EPOLLPRI ";
    if (event_bits & EPOLLOUT)
        eventstr += "EPOLLOUT ";
    if (event_bits & EPOLLRDNORM)
        eventstr += "EPOLLRDNORM ";
    if (event_bits & EPOLLRDBAND)
        eventstr += "EPOLLRDBAND ";
    if (event_bits & EPOLLWRNORM)
        eventstr += "EPOLLWRNORM ";
    if (event_bits & EPOLLWRBAND)
        eventstr += "EPOLLWRBAND ";
    if (event_bits & EPOLLMSG)
        eventstr += "EPOLLMSG ";
    if (event_bits & EPOLLERR)
        eventstr += "EPOLLERR ";
    if (event_bits & EPOLLHUP)
        eventstr += "EPOLLHUP ";
    if (event_bits & EPOLLRDHUP)
        eventstr += "EPOLLRDHUP ";
    if (event_bits & EPOLLONESHOT)
        eventstr += "EPOLLONESHOT ";
    if (event_bits & EPOLLET)
        eventstr += "EPOLLET ";
    return eventstr;
}

void udp_dispatch::wake_next_thread()
{
  struct sockaddr_in6 addr = { 0 };
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(udp_port_);
  addr.sin6_addr = in6addr_loopback;
  char buf = 0;
  
  if (sendto(udp_sock_, &buf, sizeof(buf), 0, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    anon_log_error("sendto failed with errno: " << errno_string());
}

void udp_dispatch::read_loop()
{
  anon_log("starting thread: upd_dispatch::read_loop");

  while (true)  {
    struct epoll_event event;
    int ret;
    ret = epoll_wait(ep_fd_, &event, 1, -1);
    
    if (!running_)
    {
      wake_next_thread();
      break;
    }
    
    if (ret > 0) {
    
      if (event.events & ~EPOLLIN)
        anon_log("epoll_wait returned with event bits other than EPOLLIN, bits: " << event_bits_to_string(event.events));
    
      if (event.events & EPOLLIN)
        recv_msg();
    
    } else if ((ret != 0) && (errno != EINTR)) {
    
      // if we have a debugger attached, then every time the debugger hits a break point
      // it sends signals to all the threads, and we end up coming out of epoll_wait with
      // errno set to EINTR.  That's not worth printing...
      anon_log_error("epoll_wait returned with errno = " << errno_string());
      
    }
  }
  
  anon_log("exiting thread: upd_dispatch::read_loop");
}

void udp_dispatch::stop()
{
  running_ = false;
  wake_next_thread();
  
  for (auto thread = io_threads_.begin(); thread != io_threads_.end(); ++thread)
    thread->join();
  io_threads_.clear();
  close(udp_sock_);
  close(ep_fd_);
}



