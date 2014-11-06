/*
 Copyright (c) 2014 Anon authors, see AUTHORS file.
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#include <stdio.h>
#include <thread>
#include <arpa/inet.h>
#include <netdb.h>
#include "log.h"
#include "udp_dispatch.h"
#include "big_id_serial.h"
#include "big_id_crypto.h"
#include "fiber.h"
#include "tcp_server.h"
#include "tcp_client.h"
#include "dns_cache.h"
#include "lock_checker.h"
#include "time_utils.h"
#include "http_server.h"

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
  anon_log("id: (short) " << id);
  anon_log("id: (long) " << ldisp(id));
  anon_log("random id: " << ldisp(rand_id()));
  anon_log("sha256 id: " << ldisp(sha256_id("hello world\n", strlen("hello world\n"))));
  
  
  {
    int                 udp_port = 8617;
    int                 tcp_port = 8618;
    int                 http_port = 8619;
    
    src_addr_validator  validator;
    my_udp              m_udp(udp_port,validator);
    
    io_dispatch         io_d(std::thread::hardware_concurrency(),false);
    m_udp.attach(io_d);
    fiber::attach(io_d);
    dns_cache::attach(io_d);
    
    http_server my_http(http_port, validator,
                      [](const http_server::http_request& request, http_server::http_reply& reply){
                        reply.add_header("Content-Type", "text/plain");
                        reply << "hello browser!\n\n";
                        reply << "src addr: " << *request.src_addr << "\n";
                        reply << "http version major: " << request.http_major << "\n";
                        reply << "http version minor: " << request.http_minor << "\n";
                        reply << "method: " << request.method_str() << "\n\n";
                        reply << "-- headers --\n";
                        for (auto it = request.headers.begin(); it != request.headers.end(); it++)
                          reply << " " << it->first << ": " << it->second << "\n";
                        reply << "\n";
                        reply << "url path: " << request.get_url_field(UF_PATH) << "\n";
                        reply << "url query: " << request.get_url_field(UF_QUERY) << "\n";
                      });
    my_http.attach(io_d);
    
    int num_pipe_pairs = 100;
    int num_read_writes = 10000;
    
    while (true) {
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
          anon_log("  fr - run a fiber that creates additional fibers using \"run\" start mechanism");
          anon_log("  or - similar to 'fr', except using threads instead of fibers");
          anon_log("  d  - dns cache lookup of \"www.google.com\", port 80");
          anon_log("  df - same as 'd', except initiation of lookup is done from a fiber");
          anon_log("  c  - tcp connect to \"www.google.com\", port 80 and print a message");
          anon_log("  cp - tcp connect to \"www.google.com\", port 79 and print a message - fails slowly");
          anon_log("  ch - tcp connect to \"nota.yyrealhostzz.com\", port 80 and print a message - fails quickly");
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
          io_d.schedule_task([]{anon_log("task completed");}, cur_time()+1);
        } else if (!strcmp(&msgBuff[0], "tt")) {
          anon_log("queueing one second delayed task and deleting it before it expires");
          auto t = io_d.schedule_task([]{anon_log("oops, task completed!");}, cur_time()+1);
          if (io_d.remove_task(t)) {
            anon_log("removed the task " << t);
          } else
            anon_log("failed to remove the task " << t);
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
          auto start_time = cur_time();
          
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
          
          anon_log("fiber test done, total time: " << cur_time() - start_time << " seconds");
            
        } else if (!strcmp(&msgBuff[0], "ot")) {
        
          anon_log("executing thread dispatch timing test");
          auto start_time = cur_time();
          
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
            
          anon_log("thread test done, total time: " << cur_time() - start_time << " seconds");

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
          
        } else if (!strcmp(&msgBuff[0], "fr")) {

          anon_log("starting fiber which starts 10000 sub fibers using \"run\" mechanism");
          auto start_time = cur_time();

          fiber::run_in_fiber([]{
          
            fiber_mutex mutex;
            fiber_cond cond;
            int num_fibers = 10000;
            int started = 0;
            
            // "run" start
            for (int fc=0; fc<num_fibers; fc++) {
            
              fiber::run_in_fiber([&mutex,&cond,&started,num_fibers]{
                fiber_lock lock(mutex);
                if (++started == num_fibers)  {
                  anon_log("last sub fiber, now notifying");
                  cond.notify_all();
                }
              });
              
            }
            
            fiber_lock lock(mutex);
            while (started != num_fibers)
              cond.wait(lock);
            
          });
          
          fiber::wait_for_zero_fibers();
          
          anon_log("fiber test done, total time: " << cur_time() - start_time << " seconds");
          
        } else if (!strcmp(&msgBuff[0], "or")) {
        
          anon_log("starting thread which starts 10000 sub threads");
          auto start_time = cur_time();
          
          std::thread([]{
          
            std::mutex  mutex;
            std::condition_variable cond;
            int num_threads = 10000;
            int started = 0;
            
            for (int tc=0; tc<num_threads; tc++) {
            
              std::thread([&mutex,&cond,&started,num_threads]{
                anon::unique_lock<std::mutex>  lock(mutex);
                if (++started == num_threads)  {
                  anon_log("last sub thread, now notifying");
                  cond.notify_all();
                }
              }).detach();
            
            }
            
            anon::unique_lock<std::mutex>  lock(mutex);
            while (started != num_threads)
              cond.wait(lock);
          
          }).join();
          
          anon_log("thread test done, total time: " << cur_time() - start_time << " seconds");

        } else if (!strcmp(&msgBuff[0], "c")) {
        
          const char* host = "www.google.com";
          int port = 80;
        
          anon_log("tcp connecting to \"" << host << "\", port " << port);
          tcp_client::connect_and_run(host, port, [host, port](int err_code, std::unique_ptr<fiber_pipe>&& pipe){
            if (err_code == 0)
              anon_log("connected to \"" << host << "\", port " << port << ", now disconnecting");
            else if (err_code > 0)
              anon_log("connection to \"" << host << "\", port " << port << " failed with error: " << error_string(err_code));
            else
              anon_log("connection to \"" << host << "\", port " << port << " failed with error: " << gai_strerror(err_code));
          });

        } else if (!strcmp(&msgBuff[0], "cp")) {
        
          const char* host = "127.0.0.1";
          int port = 79;
        
          anon_log("trying to tcp connect to \"" << host << "\", port " << port);
          tcp_client::connect_and_run(host, port, [host, port](int err_code, std::unique_ptr<fiber_pipe>&& pipe){
            if (err_code == 0)
              anon_log("connected to \"" << host << "\", port " << port << ", now disconnecting");
            else
              anon_log("connection to \"" << host << "\", port " << port << " failed with error: " << (err_code > 0 ? error_string(err_code) : gai_strerror(err_code)));
          });
          
        } else if (!strcmp(&msgBuff[0], "ch")) {
        
          const char* host = "nota.yyrealhostzz.com";
          int port = 80;
        
          anon_log("trying to tcp connect to \"" << host << "\", port " << port);
          tcp_client::connect_and_run(host, port, [host, port](int err_code, std::unique_ptr<fiber_pipe>&& pipe){
            if (err_code == 0)
              anon_log("connected to \"" << host << "\", port " << port << ", now disconnecting");
            else
              anon_log("connection to \"" << host << "\", port " << port << " failed with error: " << (err_code > 0 ? error_string(err_code) : gai_strerror(err_code)));
          });
          
        } else if (!strcmp(&msgBuff[0], "d")) {
        
          const char* host = "www.google.com";
          int port = 80;
        
          anon_log("looking up \"" << host << "\", port " << port << " (twice)");
          for (int i = 0; i < 2; i++)
            dns_cache::lookup_and_run(host, port, [host, port](int err_code, const struct sockaddr *addr, socklen_t addrlen){
              if (err_code == 0)
                anon_log("dns lookup for \"" << host << "\", port " << port << " found: " << *addr );
              else
                anon_log("dns lookup for \"" << host << "\", port " << port << " failed with error: " << (err_code > 0 ? error_string(err_code) : gai_strerror(err_code)));
            });
            
        } else if (!strcmp(&msgBuff[0], "df")) {
        
          const char* host = "www.google.com";
          int port = 80;
        
          anon_log("running a fiber which looks up \"" << host << "\", port " << port << " (twice)");
          fiber::run_in_fiber([host, port]{
            for (int i = 0; i < 2; i++ )
              dns_cache::lookup_and_run(host, port, [host, port](int err_code, const struct sockaddr *addr, socklen_t addrlen){
                if (err_code == 0)
                  anon_log("dns lookup for \"" << host << "\", port " << port << " found: " << *addr );
                else
                  anon_log("dns lookup for \"" << host << "\", port " << port << " failed with error: " << (err_code > 0 ? error_string(err_code) : gai_strerror(err_code)));
              });
          });

        } else
          anon_log("unknown command - \"" << &msgBuff[0] << "\", type \"h <return>\" for help");
      }
    }
  }
  
  fiber::terminate();
  term_big_id_crypto();
  
  anon_log("application exit");

  return 0;
}

