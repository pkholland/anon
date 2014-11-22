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

#include "http2.h"
#include <algorithm>

struct open_stream_handler
{
  template<typename Fn>
  open_stream_handler(Fn f, size_t stack_size, int fd)
    : fiber_(f,stack_size),
      pipe_(fd, fiber_pipe::unix_domain)
  {}
  
  fiber       fiber_;
  fiber_pipe  pipe_;
};

struct handlers_t
{
  std::map<uint32_t, std::unique_ptr<open_stream_handler>>  map;
  
  ~handlers_t()
  {
    for (auto it=map.begin(); it!=map.end(); it++) {
      close(it->second->pipe_.release());
      it->second->fiber_.join();
    }
  }
};

const char* http2::http2_name = "h2c-15-anon";

void http2::run(http_server::pipe_t& pipe)
{
  handlers_t handlers;
  
  while (true) {
  
    // read (all of) the frame header for the next frame
    unsigned char frame[k_frame_header_size];
    size_t bytes_read = 0;
    while (bytes_read < sizeof(frame))
      bytes_read += pipe.read(&frame[bytes_read], sizeof(frame)-bytes_read);
      
    // get the fields in a more managable state
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
      
        // a request to open a new stream_id. if legal, we create a new handler for it
      
        if (handlers.map.find(stream_id) != handlers.map.end()) {
          #if ANON_LOG_NET_TRAFFIC > 1
          anon_log("invalid " << (type == k_HEADERS ? "HEADERS" : "PUSH_PROMISE") << " sent to already-open stream " << stream_id);
          #endif
          return;
        }
      
        // the new handler will be reading from a different (unix-domain) socket
        // that we will forward into.  So create that here
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) != 0)
          do_error("socketpair(AF_UNIX, SOCK_STREAM | NONBLOCK | SOCK_CLOEXEC, 0, sv)");
          
        auto svv = sv[0];
        auto handv = stream_handler_factory_->new_handler();
        
        // now start the handler running in a new fiber
        handlers.map[stream_id] = std::unique_ptr<open_stream_handler>(new open_stream_handler([svv, &pipe, stream_id, handv]{
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
        anon_log("http2 frame intended for idle stream_id " << stream_id << ", of size " << frame_size);
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

void http2::format_frame(char* buf, uint32_t length, uint8_t type, uint8_t flags, uint32_t stream_id)
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

void http2::send_data()
{}

void http2::send_headers()
{}

void http2::send_priority()
{}

void http2::send_reset_stream()
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

void http2::send_settings(http_server::pipe_t& pipe, uint32_t stream_id, const std::vector<std::pair<settings_t,uint32_t>>& settings)
{
  #if defined(ANON_RUNTIME_CHECKS)
  if (settings.size() > k_num_settings_params)
    do_error("too many parameters for http2::send_settings - tried to send " << settings.size() << ", maximum allowable is " << k_num_settings_params);
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

void http2::send_push_promise()
{}

void http2::send_ping()
{}

void http2::send_goaway()
{}

void http2::send_window_update()
{}

void http2::send_continuation()
{}

////////////////////////////////////////////

void http2::parse_settings(uint32_t stream_id, uint8_t flags, uint32_t frame_size)
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


