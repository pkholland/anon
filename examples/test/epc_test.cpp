/*
 Copyright (c) 2015 Anon authors, see AUTHORS file.
 
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

#include "epc_test.h"
#include "tcp_server.h"
#include "epc.h"

int port_nums[5];
int port_count;

class test_server : public tcp_server
{
public:
  test_server()
      : tcp_server(0 /*port*/, [](std::unique_ptr<fiber_pipe> &&pipe, const sockaddr *src_addr, socklen_t src_addr_len) {
          for (int i = 0; i < 1000; i++)
          {
            char buf[100];
            int bytes_read = 0;
            while (!bytes_read || buf[bytes_read - 1])
              bytes_read += pipe->read(&buf[bytes_read], sizeof(buf) - bytes_read);
            const char *reply = "world";
            //anon_log("epc server received \"" << &buf[0] << "\", replying with \"" << reply << "\"");
            //fiber::msleep(10);
            pipe->write(reply, strlen(reply) + 1);
          }
        })
  {
    port_nums[port_count++] = get_port();
  }
};

std::vector<std::unique_ptr<test_server>> my_test_servers;

void epc_test_init()
{
  for (int i = 0; i < sizeof(port_nums) / sizeof(port_nums[0]); i++)
    my_test_servers.push_back(std::unique_ptr<test_server>(new test_server()));
}

void epc_test_term()
{
  for (int i = 0; i < sizeof(port_nums) / sizeof(port_nums[0]); i++)
    my_test_servers[i]->stop();
}

void epc_test()
{
  //auto start_time = cur_time();
  fiber::run_in_fiber([] {
    auto start_time = cur_time();

    auto epc = endpoint_cluster::create("127.0.0.1", port_nums[0]);

    fiber_mutex mtx;
    fiber_cond cond;
    int num_fibers = 100;
    int remaining = num_fibers;

    for (int i = 0; i < num_fibers; i++)
    {
      fiber::run_in_fiber([&mtx, &cond, &remaining, &epc, i] {
        for (int j = 0; j < 100; j++)
          epc->with_connected_pipe([j](const pipe_t *pipe) -> bool {
            const char *msg = "hello";
            //anon_log("sending " << j << "th epc test message \"" << msg << "\"");
            pipe->write(msg, strlen(msg) + 1);
            char response[100];
            int bytes_read = 0;
            while (!bytes_read || response[bytes_read - 1])
              bytes_read += pipe->read(&response[bytes_read], sizeof(response) - bytes_read);
            //anon_log("got back \"" << &response[0] << "\"");
            return true;  // cache the socket
          });

        fiber_lock lock(mtx);
        if (--remaining == 0)
          cond.notify_one();
      });
    }
    fiber_lock lock(mtx);
    while (remaining)
      cond.wait(lock);

    anon_log("finished " << num_fibers * 100 << " connections in " << to_seconds(cur_time() - start_time) << " seconds");
  });
}
