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

#include "http2_handler.h"

void http2_handler::exec(http_server::pipe_t &pipe, const http_request &request)
{
  // upgrade to HTTP/2 is only legal if the client is at least HTTP/1.1
  // this is a conclusion from the fact that the spec requires us to
  // send a 101 back, and 101's are not defined prior to HTTP/1.1, so
  // are invalid to send back.
  if (request.http_major < 1 || request.http_minor < 1)
  {
#if ANON_LOG_NET_TRAFFIC > 1
    anon_log("ignoring http/2 upgrade sent from " << *request.src_addr << ", because it was not HTTP/1.1, so illegal.  Was only HTTP/" << request.http_major << "." << request.http_minor);
#endif
    return;
  }

  // section 3.2.1 of the spec.
  // this isn't quite right here.  We disconnect the socket if the header is not
  // present. the spec seems to suggest that it simply not upgrade -- which we aren't
  // doing -- but probably doesn't imply that we should disconnect.
  if (!request.headers.contains_header("HTTP2-Settings"))
  {
#if ANON_LOG_NET_TRAFFIC > 1
    anon_log("ignoring http/2 upgrade sent from " << *request.src_addr << ", because it does not contain an HTTP2-Settings header");
#endif
    return;
  }

  // acknowledg the 1.x -> 2 upgrade
  http_response resp;
  resp.set_status_code("101 Switching Protocols");
  resp.add_header("Connection", "Upgrade");
  resp.add_header("Upgrade", http2::http2_name);
  pipe.respond(resp);

  // send the server "preface" (section 3.5)
  // unclear to me what stream_id these are supposed to be sent on.  I am
  // assuming that they are sent on stream_id 1.  The last sentence of
  // section 3.2 is:
  //
  //   After commencing the HTTP/2 connection, stream 1 is used for the response
  //
  http2_.send_settings(pipe, /*stream_id*/ 1);

  // TODO - support initial body in request (that is the stuff "prior to commencing HTTP/2")

  // we have now switched to HTTP/2, so just run the handler
  http2_.run(pipe);
}
