
#include <stdio.h>
#include <thread>
#include "log.h"
#include "udp_dispatch.h"
#include "big_id_serial.h"
#include "big_id_crypto.h"

static void handle_msg(const unsigned char* msg, ssize_t len,
                       const struct sockaddr_storage *sockaddr,
                       socklen_t sockaddr_len)
{
  anon_log("received msg of: \"" << (char*)msg << "\"");
}

extern "C" int main(int argc, char** argv)
{
  anon_log("application start");
  
  if (!init_big_id_crypto())  {
    anon_log_error("init_big_id_crypto failed");
    return 1;
  }
  
  uint8_t id_data[32] = {0, 1, 2, 3, 4, 5, 6, 7,
                         8,  9, 10, 11, 12, 13, 14, 15,
                        16, 17, 18, 19, 20, 21, 22, 23,
                        24, 25, 26, 27, 28, 29, 30, 31};
  big_id id(id_data);
  anon_log("id: (short) " << id << " (long) " << ldisp(id));
  anon_log("random id: " << ldisp(rand_id()));
  anon_log("sha256 id: " << ldisp(sha256_id("hello world", strlen("hello world"))));

  udp_dispatch::init(std::thread::hardware_concurrency(),8617,handle_msg,false);
  
  udp_dispatch::test_msg();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  
  udp_dispatch::stop();
  
  term_big_id_crypto();
  
  anon_log("application exit");

	return 0;
}

