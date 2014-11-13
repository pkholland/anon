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

#include "io_dispatch.h"
#include "fiber.h"
#include "http_server.h"

extern "C" int main(int argc, char** argv)
{
  if (argc != 2)
  {
    printf("usage: echo <port>\n");
    return 1;
  }
  
  int http_port = atoi(argv[1]);
  anon_log("starting http server on port " << http_port);

  io_dispatch::start(std::thread::hardware_concurrency(), false);
  fiber::initialize();
  
  http_server my_http(http_port,
                    [](http_server::pipe_t& pipe, const http_request& request){
                      http_response response;
                      response.add_header("Content-Type", "text/plain");
                      response << "echo server!\n";
                      response << "your url query was: " << request.get_url_field(UF_QUERY) << "\n";
                      pipe.respond(response);
                    });

  while (true) {
    // read a command from stdin
    char msgBuff[256];
    auto bytes_read = read(0/*stdin*/,&msgBuff[0],sizeof(msgBuff));
    
    break;
  }

  io_dispatch::join();
  fiber::terminate();

  anon_log("stopping server and exiting");
  return 0;
}

