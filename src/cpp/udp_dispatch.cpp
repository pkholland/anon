
#include <udp_dispatch>
#include <sys/epoll.h>
#include <system_error>

void udp_dispatch::initialize(int num_threads, int max_num_endpoints, int port,
                        int udp_port, message_handler_function message_handler,
                        int socket_inactive_timeout, int sweep_interval)
{
  ep_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (ep_fd_ < 0)
  {
    anon_log("epoll_create1 failed, errno: " << errno);
    throw new std::system_error(errno, std::system_category());
  }
}


