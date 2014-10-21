
#pragma once

#include <sys/socket.h>
#include <vector>
#include <thread>

class udp_dispatch
{
public:

  typedef void (*message_handler_function)( const unsigned char* msg, ssize_t len,
                                            const struct sockaddr_storage *sockaddr,
                                            socklen_t sockaddr_len);

  static void init(int num_threads, int udp_port,
                        message_handler_function message_handler,
                        bool do_inline);

  /** Stop accepting new connections */
  static void stop();
  
  static void test_msg();

private:
  static void init_udp_socket();
  static void recv_msg(bool firstTime = false);
  static void read_loop();
  static void wake_next_thread();

  static bool running_;
  static int ep_fd_;
  static int udp_sock_;
  static int udp_port_;
  static message_handler_function message_handler_;
  static std::vector<std::thread> io_threads_;


};

