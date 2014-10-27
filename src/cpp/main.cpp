
#include <stdio.h>
#include <thread>
#include <arpa/inet.h>
#include "log.h"
#include "udp_dispatch.h"
#include "big_id_serial.h"
#include "big_id_crypto.h"

class my_udp : public udp_dispatch
{
public:
  my_udp(int port, const src_addr_validator& validator)
    : udp_dispatch(port, validator)
  {}

  virtual void recv_msg(const unsigned char* msg, ssize_t len,
                        const struct sockaddr_storage *sockaddr,
                        socklen_t sockaddr_len)
  {
    anon_log("received msg of: \"" << (char*)msg << "\"");
    //std::this_thread::sleep_for(std::chrono::milliseconds( 0 ));
  }

};

class my_task : public io_dispatch::scheduled_task
{
public:
    virtual void exec()
    {
      anon_log("task completed");
      delete this;
    }
};

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
  anon_log("sha256 id: " << ldisp(sha256_id("hello world\n", strlen("hello world\n"))));
  
  
  {
    int                 udp_port = 8617;
    src_addr_validator  validator;
    my_udp              m_udp(udp_port,validator);
    io_dispatch         io_d(std::thread::hardware_concurrency(),false);
    m_udp.attach(io_d);
    
    while (true)
    {
      // read a command from stdin
      char msgBuff[256];
      auto bytes_read = read(0/*stdin*/,&msgBuff[0],sizeof(msgBuff));
      
      if (bytes_read > 1) {
        // truncate the return char to a 0 so we can compare to strings
        msgBuff[bytes_read-1] = 0;
      
        if (!strcmp(&msgBuff[0], "q")) {
          anon_log("quitting");
          break;
        } else if (!strcmp(&msgBuff[0], "h")) {
          anon_log("available commands:");
          anon_log("  q  - quit");
          anon_log("  p  - pause all io threads, print while paused, then resume");
          anon_log("  s  - send some udp packets to the udp handler");
          anon_log("  h  - display this menu");
          anon_log("  t  - install a one second timer, which when it expires prints a message");
          anon_log("  tt - schedule, and then delete a timer before it has a chance to expire");
          anon_log("  e  - execute a print statement once on each io thread");
        } else if (!strcmp(&msgBuff[0], "p")) {
          anon_log("pausing io threads");
          io_d.while_paused([]{anon_log("all io threads now paused");});
          anon_log("resuming io threads");
        } else if (!strcmp(&msgBuff[0], "s")) {
          int num_messages = 20;
          anon_log("sending " << num_messages << " udp packet" << (num_messages == 1 ? "" : "s") << " to my_udp on loopback addr");
          
          struct sockaddr_in6 addr = { 0 };
          addr.sin6_family = AF_INET6;
          addr.sin6_port = htons(udp_port);
          addr.sin6_addr = in6addr_loopback;

          for (int i=0; i<num_messages; i++) {
            std::ostringstream msg;
            msg << "hello world (" << std::to_string(i) << ")" /*<< rand_id()*/;
            if (sendto(m_udp.get_sock(), msg.str().c_str(), strlen(msg.str().c_str()) + 1, 0, (struct sockaddr *)&addr, sizeof(addr)) == -1)
              anon_log_error("sendto failed with errno: " << errno_string());
          }
        } else if (!strcmp(&msgBuff[0], "t")) {
          anon_log("queueing one second delayed task");
          struct timespec t_spec;
          t_spec.tv_sec = 1;
          t_spec.tv_nsec = 0;
          io_d.schedule_task(new my_task(), t_spec);
        } else if (!strcmp(&msgBuff[0], "tt")) {
          anon_log("queueing one second delayed task and deleting it before it expires");
          struct timespec t_spec;
          t_spec.tv_sec = 1;
          t_spec.tv_nsec = 0;
          auto t = new my_task;
          io_d.schedule_task(t, t_spec);
          if (io_d.remove_task(t)) {
            anon_log("removed the task");
            delete t;
          } else
            anon_log("failed to remove the task");
        } else if (!strcmp(&msgBuff[0], "e")) {
          anon_log("executing print statement on each io thread");
          io_d.on_each([]{anon_log("hello from io thread " << syscall(SYS_gettid));});
        }
        else
          anon_log("unknown command - \"" << &msgBuff[0] << "\", type \"h<return>\" for help");
      }
    }
  }
  
  term_big_id_crypto();
  
  anon_log("application exit");

  return 0;
}

