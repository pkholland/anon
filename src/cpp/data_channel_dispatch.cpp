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

#include "data_channel_dispatch.h"
#include "log.h"
#include "big_endian_access.h"

#if 0
// "payload protocol identifiers" in the "data chunks" of SCTP messages
// RFC 8831, section 8
Value                               SCTP PPID         Reference     Date
WebRTC String                       51                RFC 8831      2013-09-20
WebRTC Binary Partial (deprecated)  52                RFC 8831      2013-09-20
WebRTC Binary                       53                RFC 8831      2013-09-20
WebRTC String Partial (deprecated)  54                RFC 8831      2013-09-20
WebRTC String Empty                 56                RFC 8831      2014-08-22
WebRTC Binary Empty                 57                RFC 8831      2014-08-22

WebRTC DCEP (50) - DCEP messages.
#endif

enum {
  PPID_DCEP = 50,
  PPID_String = 51,
  PPID_Binary = 53,
  PPID_EmptyString = 56,
  PPID_EmptyBinary = 57
};

data_channel_dispatch::data_channel_stream::data_channel_stream(
  int channel_type,
  int priority, 
  int reliability,
  std::string&& label,
  std::string&& protocol)
  : channel_type(channel_type),
    priority(priority),
    reliability(reliability),
    label(std::move(label)),
    protocol(std::move(protocol))
{}

void data_channel_dispatch::data_channel_stream::do_data(
  data_channel_dispatch* cd,
  uint32_t tsn,
  uint16_t stream_sequency_num,
  uint32_t ppid,
  const uint8_t* data, size_t len)
{
  switch (ppid) {
    case PPID_String:
      anon_log("\ngot PPID_String:\n"
          << "string: \"" << std::string((char*)data, len) << "\"\n"
          << "stream_sequency_num: " << stream_sequency_num << "\n"
          << "tsn: " << tsn);
      cd->add_chunk(tsn, nullptr, 0);
      break;
    case PPID_Binary:
    case PPID_EmptyString:
    case PPID_EmptyBinary:
      break;
  }
}


data_channel_dispatch::data_channel_dispatch(std::function<void(uint32_t tsn, const uint8_t* msg, size_t len)> add_chunk)
  : add_chunk(std::move(add_chunk))
{}


void data_channel_dispatch::do_dcep(uint32_t tsn, int stream_id, const uint8_t* data, size_t len)
{
  // WebRTC DCEP messages support only either DATA_CHANNEL_OPEN (0x03)
  // and DATA_CHANNEL_ACK (0x02)
  #if 0
  // RFC 8832, section 8.2.1
  Name              Type      Reference
  Reserved	        0x00      RFC 8832
  Reserved          0x01      RFC 8832
  DATA_CHANNEL_ACK  0x02      RFC 8832
  DATA_CHANNEL_OPEN 0x03      RFC 8832
  Unassigned        0x04-0xfe	
  Reserved          0xff      RFC 8832
  #endif

  // the DATA_CHANNEL_OPEN message
  #if 0
  // RFC 8832, section 5.1
  // 
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |  Message Type |  Channel Type |            Priority           |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                    Reliability Parameter                      |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |         Label Length          |       Protocol Length         |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  \                                                               /
  |                             Label                             |
  /                                                               \
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  \                                                               /
  |                            Protocol                           |
  /                                                               \
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  #endif

  // the DATA_CHANNEL_ACK message
  #if 0
  // RFC 8832, section 5.2
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |  Message Type |
  +-+-+-+-+-+-+-+-+
  #endif

  if (len > 0) {
    switch (data[0]) {
      case 0x03:  //DATA_CHANNEL_OPEN
        if (len >= 12) {
          auto channel_type = data[1];
          auto priority = get_be_uint16(&data[2]);
          auto reliability = get_be_uint32(&data[4]);
          auto label_len = get_be_uint16(&data[8]);
          auto protocol_len = get_be_uint16(&data[10]);
          auto required_len = 12 + label_len + protocol_len;
          if (len >= required_len) {
            auto label = std::string((char*)&data[12], label_len);
            auto protocol = std::string((char*)&data[12+label_len], protocol_len);
            auto stream = std::make_shared<data_channel_stream>(channel_type, priority, reliability, std::move(label), std::move(protocol));
            streams.emplace(std::make_pair(stream_id, stream));
            uint8_t ack = 0x02;
            add_chunk(tsn, &ack, sizeof(ack));
          }
          else {
            anon_log("dcep record too short");
          }
        }
        else {
          anon_log("dcep record too short");
        }
        break;
      case 0x02:  // DATA_CHANNEL_ACK
        break;
    }
  }
}

void data_channel_dispatch::recv_data_chunk(const uint8_t *chunk, ssize_t chunk_len)
{
  #if 0
  // RFC 4960, section 3.3.1
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |   Type = 0    | Reserved|U|B|E|    Length                     |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                              TSN                              |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |      Stream Identifier S      |   Stream Sequence Number n    |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                  Payload Protocol Identifier                  |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  \                                                               \
  /                 User Data (seq n of Stream S)                 /
  \                                                               \
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  #endif
  if (chunk_len < k_data_chunk_header_size)
  {
    anon_log("data chunk header too small");
  }
  else {
    auto unordered = (chunk[1] & 0x04) != 0;
    auto beginning = (chunk[1] & 0x02) != 0;
    auto end = (chunk[1] & 0x01) != 0;
    auto tsn = get_be_uint32(&chunk[4]);
    auto stream_id = get_be_uint16(&chunk[8]);
    auto stream_seq_num = get_be_uint16(&chunk[10]);
    auto ppid = get_be_uint32(&chunk[12]);

    switch(ppid) {
      case PPID_DCEP:
        do_dcep(tsn, stream_id, &chunk[k_data_chunk_header_size], chunk_len - k_data_chunk_header_size);
        break;
      case PPID_String:
      case PPID_Binary:
      case PPID_EmptyString:
      case PPID_EmptyBinary:
        {
          auto stream_it = streams.find(stream_id);
          if (stream_it != streams.end()) {
            // is the chunk complete?
            if (beginning && end) {
              stream_it->second->do_data(this, tsn, stream_seq_num, ppid, &chunk[k_data_chunk_header_size], chunk_len - k_data_chunk_header_size);
            }
            else {
              anon_log("TODO: need logic to put chunks together");
            }
          }
          else {
            anon_log("unknown stream id: " << stream_id);
          }
        }
        break;
      default:
        anon_log("uknown payload protocol id: " << ppid);
    }
  }

}

