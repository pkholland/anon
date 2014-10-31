
#include <stdio.h>
#include <thread>
#include <arpa/inet.h>
#include "log.h"
#include "udp_dispatch.h"
#include "big_id_serial.h"
#include "big_id_crypto.h"
#include "fiber.h"

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

std::string to_string(const struct timespec& spec)
{
  std::ostringstream str;
  str << spec.tv_sec << ".";
  
  std::ostringstream dec;
  dec << std::setiosflags(std::ios_base::right) << std::setfill('0') << std::setw(3) << (spec.tv_nsec / 1000000);
  
  str << dec.str();
  return str.str();
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
  anon_log("sha256 id: " << ldisp(sha256_id("hello world\n", strlen("hello world\n"))));
  
  
  {
    int                 udp_port = 8617;
    src_addr_validator  validator;
    my_udp              m_udp(udp_port,validator);
    io_dispatch         io_d(std::thread::hardware_concurrency(),false);
    m_udp.attach(io_d);
    fiber::attach(io_d);
    
    int num_pipe_pairs = 100;
    int num_read_writes = 10000;
    
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
          anon_log("  o  - execute a print statement once on a single io thread");
          anon_log("  f  - execute a print statement on a fiber");
          anon_log("  ft - test how long it takes to fiber/context switch " << num_pipe_pairs * num_read_writes << " times");
          anon_log("  ot - similar test to 'ft', except run in os threads to test thread dispatch speed");
          anon_log("  fi - run a fiber that creates additional fibers using \"in-fiber\" start mechanism");
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
        } else if (!strcmp(&msgBuff[0], "o")) {
          anon_log("executing print statement on one io thread");
          io_d.on_one([]{anon_log("hello from io thread " << syscall(SYS_gettid));});
        } else if (!strcmp(&msgBuff[0], "f")) {
          anon_log("executing print statement from a fiber");
          fiber::run_in_fiber([]{anon_log("hello from fiber " << get_current_fiber_id());});
        } else if (!strcmp(&msgBuff[0], "ft")) {
        
          anon_log("executing fiber dispatch timing test");
          struct timespec start_time;
          if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0)
            do_error("clock_gettime(CLOCK_MONOTONIC, &start_time)");
          
          std::vector<int> fds(num_pipe_pairs*2);
          for (int i=0; i<num_pipe_pairs; i++) {
            if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, &fds[i*2]) != 0)
              do_error("socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv)");
          }
          for (int i=0; i<num_pipe_pairs; i++) {
            fiber::run_in_fiber([fds,i,num_read_writes]{
              fiber_pipe pipe(fds[i*2],fiber_pipe::unix_domain);
              for (int rc=0; rc<num_read_writes; rc++) {
                int v;
                pipe.read(&v,sizeof(v));
                if (v != rc)
                  anon_log("fiber read " << v << " instead of " << rc);
              }
            });
          }
          fiber::run_in_fiber([fds,num_pipe_pairs,num_read_writes]{
            std::vector<std::unique_ptr<fiber_pipe> > pipes;
            for (int pc=0; pc<num_pipe_pairs; pc++)
              pipes.push_back(std::move(std::unique_ptr<fiber_pipe>(new fiber_pipe(fds[pc*2+1],fiber_pipe::unix_domain))));
            for (int wc=0; wc<num_read_writes; wc++) {
              for (int pc=0; pc<num_pipe_pairs; pc++)
                pipes[pc]->write(&wc,sizeof(wc));
            }
          });
          
          fiber::wait_for_zero_fibers();
          
          struct timespec end_time;
          if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0)
            do_error("clock_gettime(CLOCK_MONOTONIC, &end_time)");
          anon_log("fiber test done, total time: " << to_string(end_time - start_time) << " seconds");
            
        } else if (!strcmp(&msgBuff[0], "ot")) {
        
          anon_log("executing thread dispatch timing test");
          struct timespec start_time;
          if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0)
            do_error("clock_gettime(CLOCK_MONOTONIC, &start_time)");
          
          std::vector<int>          fds(num_pipe_pairs*2);
          std::vector<std::thread>  threads;
          for (int i=0; i<num_pipe_pairs; i++) {
            if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, &fds[i*2]) != 0)
              anon_log_error("socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) failed with errno: " << errno_string());
          }
          for (int i=0; i<num_pipe_pairs; i++) {
            threads.push_back(std::thread([fds,i,num_read_writes]{
              try {
                for (int rc=0; rc<num_read_writes; rc++) {
                  int v;
                  auto bytes_read = read(fds[i*2],&v,sizeof(v));
                  if (bytes_read != sizeof(v))
                    anon_log_error("read(" << fds[i*2] << ",...) returned " << bytes_read << ", errno: " << errno_string());
                  else if (v != rc)
                    anon_log_error("thread read " << v << " instead of " << rc);
                }
                close(fds[i*2]);
              } catch (const std::exception& err) {
                anon_log_error("exception caught, what = " << err.what());
              } catch (...) {
                anon_log_error("unknown exception caught");
              }
            }));
          }
          threads.push_back(std::thread([fds,num_pipe_pairs,num_read_writes]{
            try {
              for (int wc=0; wc<num_read_writes; wc++) {
                for (int pc=0; pc<num_pipe_pairs; pc++) {
                  if (write(fds[pc*2+1],&wc,sizeof(wc)) != sizeof(wc))
                    anon_log_error("(write(fds[pc*2+1],&wc,sizeof(wc)) failed with errno: " << errno_string());
                }
              }
              for (int pc=0; pc<num_pipe_pairs; pc++)
                close(fds[pc*2+1]);
            } catch (const std::exception& err) {
              anon_log_error("exception caught, what = " << err.what());
            } catch (...) {
              anon_log_error("unknown exception caught");
            }
          }));
          
          for (auto thread = threads.begin(); thread != threads.end(); ++thread)
            thread->join();
          struct timespec end_time;
          
          if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0)
            do_error("clock_gettime(CLOCK_MONOTONIC, &end_time)");
          anon_log("thread test done, total time: " << to_string(end_time - start_time) << " seconds");

        } else if (!strcmp(&msgBuff[0], "fi")) {
        
          anon_log("starting fiber which starts other fibers using \"in-fiber\" mechanism");
          
          fiber::run_in_fiber([]{
            fiber_mutex mutex;
            mutex.lock();
            anon_log("start, mutex " << &mutex << " is locked");
            
            // "in-fiber" start sf1
            fiber sf1([&mutex]{
              anon_log("locking mutex " << &mutex);
              fiber_lock lock(mutex);
              anon_log("locked mutex, now unlocking and exiting");
            });
            
            // "in-fiber" start sf2
            fiber sf2([&mutex]{
              anon_log("locking mutex " << &mutex);
              fiber_lock lock(mutex);
              anon_log("locked mutex, now unlocking and exiting");
            });
            
            anon_log("fibers " << sf1.get_fiber_id() << " and " << sf2.get_fiber_id() << " both running, now unlocking mutex " << &mutex);
            mutex.unlock();
            
            // wait for sf1 and sf2 to exit
            sf1.join();
            sf2.join();
            
            anon_log("fibers " << sf1.get_fiber_id() << " and " << sf2.get_fiber_id() << " have exited");
            
          });

          fiber::wait_for_zero_fibers();
          anon_log("all fibers done");
          
        } else
          anon_log("unknown command - \"" << &msgBuff[0] << "\", type \"h<return>\" for help");
      }
    }
  }
  
  term_big_id_crypto();
  
  anon_log("application exit");

  return 0;
}

