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

#include "http2_test.h"
#include "http2_client.h"

void run_http2_test(int http_port)
{
  anon_log("sending http/2 upgrade to localhost:" << http_port);
  http2_client::connect_and_run("localhost", http_port, [](http_server::pipe_t &pipe) {
    anon_log("upgrade request succeeded!");

    http2 h2(true, [](std::unique_ptr<fiber_pipe> &&read_pipe, http_server::pipe_t &write_pipe, uint32_t stream_id) {
      anon_log("new HEADERS or PUSH_PROMISE for stream_id: " << stream_id);
    });

    fiber cf([&h2, &pipe] {
      h2.run(pipe);
    });

    std::vector<http2::hpack_header> req_headers;
    req_headers.push_back(http2::hpack_header(":method", "GET"));
    req_headers.push_back(http2::hpack_header(":scheme", "http"));
    req_headers.push_back(http2::hpack_header(":path", "/"));
    h2.open_stream(pipe, req_headers, true);
  });
}
