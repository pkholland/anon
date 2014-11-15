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

#include "http2_handler.h"
#include <algorithm>

const char* http2_handler::http2_name = "h2c-15-anon";

struct running_stream_handler
{
  template<typename Fn>
  running_stream_handler(Fn f, size_t stack_size, int fd)
    : fiber_(f,stack_size),
      pipe_(fd, fiber_pipe::unix_domain)
  {}
  
  fiber       fiber_;
  fiber_pipe  pipe_;
};

struct handlers_t
{
  std::map<uint32_t, std::unique_ptr<running_stream_handler> >  map;
  
  ~handlers_t()
  {
    for (auto it=map.begin(); it!=map.end(); it++) {
      close(it->second->pipe_.release());
      it->second->fiber_.join();
    }
  }
};

void http2_handler::exec(http_server::pipe_t& pipe, const http_request& request)
{
  // upgrade to HTTP/2 is only legal if the client is at least HTTP/1.1
  // this is a conclusion from the fact that the spec requires us to
  // send a 101 back, and 101's are not defined prior to HTTP/1.1, so
  // are invalid to send back.
  if (request.http_major < 1 || request.http_minor < 1) {
    #if ANON_LOG_NET_TRAFFIC > 1
    anon_log("ignoring http/2 upgrade sent from " << *request.src_addr << ", because it was not HTTP/1.1, so illegal.  Was only HTTP/" << request.http_major << "." << request.http_minor);
    #endif
    return;
  }
  
  // section 3.2.1 of the spec.
  // this isn't quite right here.  We disconnect the socket if the header is not
  // present. the spec seems to suggest that it simply not upgrade -- which we aren't
  // doing -- but probably doesn't imply that we should disconnect.
  if (!request.contains_header("HTTP2-Settings")) {
    #if ANON_LOG_NET_TRAFFIC > 1
    anon_log("ignoring http/2 upgrade sent from " << *request.src_addr << ", because it does not contain an HTTP2-Settings header");
    #endif
    return;
  }
    
  // acknowledging the 1.x -> 2 upgrade
  http_response resp;
  resp.set_status_code("101 Switching Protocols");
  resp.add_header("Connection", "Upgrade");
  resp.add_header("Upgrade", http2_name);
  pipe.respond(resp);
  
  // send the server "preface" (section 3.5)
  // unclear to me what stream_id these are supposed to be sent on.  I am
  // assuming that they are sent on stream_id 1.  The last sentence of
  // section 3.2 is:
  //
  //   After commencing the HTTP/2 connection, stream 1 is used for the response
  //
  send_settings(pipe, /*stream_id*/1);
  
  // TODO - support initial body in request (that is the stuff "prior to commencing HTTP/2")
 
  handlers_t handlers;
  
  while (true) {
  
    unsigned char frame[k_frame_header_size];
    size_t bytes_read = 0;
    while (bytes_read < sizeof(frame))
      bytes_read += pipe.read(&frame[bytes_read], sizeof(frame)-bytes_read);
      
    uint32_t  netv = 0;
    memcpy(&((char*)&netv)[1], &frame[0], 3);
    uint32_t  frame_size = ntohl(netv);
    memcpy(&netv, &frame[5], 4);
    uint32_t  stream_id = ntohl(netv);
    uint8_t   type = frame[3];
    uint8_t   flags = frame[4];
    char      buf[1024];
    
    if (stream_id < 2) {
        
    } else {
 
      if (type == k_HEADERS || type == k_PUSH_PROMISE) {
      
        if (handlers.map.find(stream_id) != handlers.map.end()) {
          #if ANON_LOG_NET_TRAFFIC > 1
          anon_log("invalid " << (type == k_HEADERS ? "HEADERS" : "PUSH_PROMISE") << " sent to already-open stream " << stream_id << " from " << *request.src_addr);
          #endif
          return;
        }
      
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) != 0)
          do_error("socketpair(AF_UNIX, SOCK_STREAM | NONBLOCK | SOCK_CLOEXEC, 0, sv)");
          
        int svv = sv[0];
        auto handv = stream_handler_factory_->new_handler();
        
        handlers.map[stream_id] = std::unique_ptr<running_stream_handler>( new running_stream_handler([svv, &pipe, stream_id, handv]{
          std::unique_ptr<stream_handler> hd(handv);
          handv->exec(svv, pipe, stream_id);
        }, stack_size_, sv[1]));
        
      }
   
      auto sh = handlers.map.find(stream_id);
      if (sh != handlers.map.end()) {
        sh->second->pipe_.write(&frame[0], sizeof(frame));
        size_t  bytes_read = 0;
        while (bytes_read < frame_size) {
          auto br = pipe.read(&buf[0], std::min(sizeof(buf), frame_size-bytes_read));
          sh->second->pipe_.write(&buf[0], br);
          bytes_read += br;
        }
      } else {
        #if ANON_LOG_NET_TRAFFIC > 1
        anon_log("http2 frame intended for idle stream_id " << stream_id << ", of size " << frame_size << " from " << *request.src_addr);
        #endif
        // TODO send GOAWAY? (see 5.4.1)
        return;
      }
    
    }
    
#if 0
    switch (frame[3]) {
      case k_DATA:
        break;
      case k_HEADERS:
        break;
      case k_PRIORITY:
        break;
      case k_RST_STREAM:
        break;
      case k_SETTINGS:
        parse_settings(stream_id, flags, frame_size);
        break;
      case k_PUSH_PROMISE:
        break;
      case k_PING:
        break;
      case k_GOAWAY:
        break;
      case k_WINDOW_UPDATE:
        break;
      case k_CONTINUATION:
        break;
      default:  {
        #if ANON_LOG_NET_TRAFFIC > 1
        anon_log("ignoring unknown http2 frame type of " << (int)frame[3] << ", of size " << frame_size << " from " << *request.src_addr);
        #endif
        char    buf[512];
        size_t  bytes_skipped = 0;
        while (bytes_skipped < frame_size)
          bytes_skipped = pipe.read(&buf[0], std::min(sizeof(buf), frame_size-bytes_skipped));
      } break;
    }
#endif
  
  }
}


#if 0

Frame Layout from section 4.1 of the spec:

     0                   1                   2                   3
     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                 Length (24)                   |
    +---------------+---------------+---------------+
    |   Type (8)    |   Flags (8)   |
    +-+-+-----------+---------------+-------------------------------+
    |R|                 Stream Identifier (31)                      |
    +=+=============================================================+
    |                   Frame Payload (0...)                      ...
    +---------------------------------------------------------------+


#endif

void http2_handler::format_frame(char* buf, uint32_t length, uint8_t type, uint8_t flags, uint32_t stream_id)
{
  #if defined(ANON_RUNTIME_CHECKS)
  if (length > 0x00ffffff)
    do_error("http2 frame length greater than 0x00ffffff");
  #endif
  
  uint32_t netv = htonl(length);
  memcpy(&buf[0], ((char*)&netv)+1, 3);
  buf[3] = (char)type;
  buf[4] = (char)flags;
  
  #if defined(ANON_RUNTIME_CHECKS)
  if (stream_id & 0x80000000)
    do_error("http2 invalid stream id");
  #endif
  
  netv = htonl(stream_id);
  memcpy(&buf[5], &netv, 4);
}

void http2_handler::send_data()
{}

void http2_handler::send_headers()
{}

void http2_handler::send_priority()
{}

void http2_handler::send_reset_stream()
{}

#if 0
Settings format from section 6.5.1.

Settings frame is an array of:

     0                   1                   2                   3
     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |       Identifier (16)         |
    +-------------------------------+-------------------------------+
    |                        Value (32)                             |
    +---------------------------------------------------------------+
    

#endif

void http2_handler::send_settings(http_server::pipe_t& pipe, uint32_t stream_id, const std::vector<std::pair<settings_t,uint32_t> >& settings)
{
  #if defined(ANON_RUNTIME_CHECKS)
  if (settings.size() > k_num_settings_params)
    do_error("too many parameters for http2_handler::send_settings - tried to send " << settings.size() << ", maximum allowable is " << k_num_settings_params);
  #endif
  
  char  buf[k_frame_header_size + k_num_settings_params*6];
  
  format_frame(&buf[0], settings.size()*6, k_SETTINGS, 0/*flags*/, stream_id);
  
  for (int i=0; i<settings.size(); i++) {
    uint16_t netv16 = htons(settings[i].first);
    memcpy(&buf[k_frame_header_size + i*6 + 0], &netv16, 2);
    uint32_t netv32 = htonl(settings[i].second);
    memcpy(&buf[k_frame_header_size + i*6 + 2], &netv16, 4);
  }
  
  pipe.write(&buf[0], k_frame_header_size + settings.size()*6);
}

void http2_handler::send_push_promise()
{}

void http2_handler::send_ping()
{}

void http2_handler::send_goaway()
{}

void http2_handler::send_window_update()
{}

void http2_handler::send_continuation()
{}

////////////////////////////////////////////

void http2_handler::parse_settings(uint32_t stream_id, uint8_t flags, uint32_t frame_size)
{
  // is this an ack or an initiated settings?
  if (flags & k_settings_ack) {
    // peer is acknowleding a previous settings that we sent
    // nothing to here yet...
  } else {
    // peer is sending us a new settings, we must acknowledge it
    char  buf[k_frame_header_size];
    format_frame(&buf[0], 0/*frame size*/, k_SETTINGS, k_settings_ack, stream_id);
    
  }
}


