
#pragma once

class udp_dispatch
{
public:

  static void initialize(int num_threads, int max_num_endpoints, int port,
                        int udp_port, message_handler_function message_handler,
                        int socket_inactive_timeout, int sweep_interval);

  /** Start accepting & handling connections */
  static void start();

  /** Stop accepting new connections */
  static void stop();

private:
  static int ep_fd_;


};

