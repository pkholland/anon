/*
 Copyright (c) 2015 ANON authors, see AUTHORS file.
 
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

#include "http_server.h"


void server_init()
{
}


// example code - just returns a canned blob of text
void server_respond(http_server::pipe_t &pipe, const http_request &request, bool is_tls)
{
  http_response response;
  response.add_header("Content-Type", "text/plain");
  response << "Hello from Teflon!\n";
  response << "your url query was: " << request.get_url_field(UF_QUERY) << "\n";
  response << "server response generated from:\n";
  response << "    process: " << getpid() << "\n";
  response << "    thread:  " << syscall(SYS_gettid) << "\n";
#if defined(ANON_LOG_FIBER_IDS)
  response << "    fiber:   " << get_current_fiber_id() << "\n";
#endif
  response << "\n\nyou sent:\n";
  response << request.method_str() << " " << request.url_str << " HTTP/" << request.http_major << "." << request.http_minor << "\n";
  for (auto it = request.headers.headers.begin(); it != request.headers.headers.end(); it++)
    response << it->first.str() << ": " << it->second.str() << "\n";
  pipe.respond(response);
}

void server_sync()
{
}

void server_term()
{
}

void server_close_outgoing()
{
}
