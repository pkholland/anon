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

#pragma once

#include "http_server.h"

class http2_handler
{
public:
  const char* http2_name = "h2c-anon";

  http2_handler(http_server& serv)
  {
    serv.add_upgrade_handler(http2_name, [this](http_server::pipe_t& pipe, const http_request& request){exec(pipe, request);});
  }
  
private:
  void exec(http_server::pipe_t& pipe, const http_request& request)
  {
    // start by acknowledging the 1.x -> 2 switch if the client is at least 1.1
    if (request.http_major >= 1 && request.http_minor >= 1) {
      http_response resp;
      resp.set_status_code("101 Switching Protocols");
      resp.add_header("Connection", "Upgrade");
      resp.add_header("Upgrade", http2_name);
      pipe.respond(resp);
    }
    
    char buff[100] = { 0 };
    pipe.read(&buff[0], sizeof(buff)-1);
    anon_log("got an http/2 upgrade, body starts with: \"" << &buff[0] << "\"!");
  }
};

