/*
 Copyright (c) 2023 Anon authors, see AUTHORS file.
 
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

#include "sctp_dispatch.h"
#include "fiber.h"
#include "big_endian_access.h"
#include "tcp_utils.h"

namespace {

void append_nibble(uint8_t nib, std::ostringstream& oss, char a = 'a')
{
  char b[2] = {0};
  if (nib < 10) {
    b[0] = '0' + nib;
  }
  else {
    b[0] = a + (nib - 10);
  }
  oss << &b[0];
}

// debugging helper
void append_byte(unsigned char b, bool add_comma, std::ostringstream& oss)
{
  oss << "0x";
  append_nibble(b >> 4, oss);
  append_nibble(b & 0x0f, oss);
  if (add_comma) {
    oss << ", ";
  }
}

// debugging helper
void append_bytes(const unsigned char* bytes, size_t len, std::ostringstream& oss)
{
  oss << "[";
  for (size_t i = 0; i < len; i++) {
    append_byte(bytes[i], i != len - 1, oss);
  }
  oss << "]";
}

enum {
  k_sctp_common_header_size = 12,
  k_sctp_chunk_header_size = 4,
  k_sctp_option_header_size = 4,
  k_init_chunk_header_size = 20,
  k_data_chunk_header_size = 16,
  k_sack_chunk_header_size = 16,

  CHNK_DATA = 0,
  CHNK_INIT = 1,
  CHNK_INIT_ACK = 2,
  CHNK_SACK = 3,
  CHNK_HEARTBEAT = 4,
  CHNK_HEARTBEAT_ACK = 5,
  CHNK_ABORT = 6,
  CHNK_SHUTDOWN = 7,
  CHNK_SHUTDOWN_ACK = 8,
  CHNK_ERROR = 9,
  CHNK_COOKIE_ECHO = 10,
  CHNK_COOKIE_ACK = 11,
  CHNK_ECNE = 12,
  CHNK_CWR = 13,
  CHNK_SHUTDOWN_COMPLETE = 14,

  OPT_FORWARD_TSN = 192,
  OPT_SUPPORTED_EXTENSIONS_FIRST_BYTE = 0x80,
  OPT_SUPPORTED_EXTENSIONS_SECOND_BYTE = 0x08,
  OPT_COOKIE = 7
};

// the crc polynomial used by sctp
// x^32+x^28+x^27+x^26+x^25+x^23+x^22+x^20+x^19+x^18+x^14+x^13+x^11+x^10+x^9+x^8+x^6+x^0
// algorithm from RFC 3309 - Appendix
const uint32_t crc32_tab[256] = {
	0x00000000, 0xf26b8303, 0xe13b70f7, 0x1350f3f4,
	0xc79a971f, 0x35f1141c, 0x26a1e7e8, 0xd4ca64eb,
	0x8ad958cf, 0x78b2dbcc, 0x6be22838, 0x9989ab3b,
	0x4d43cfd0, 0xbf284cd3, 0xac78bf27, 0x5e133c24,
	0x105ec76f, 0xe235446c, 0xf165b798, 0x030e349b,
	0xd7c45070, 0x25afd373, 0x36ff2087, 0xc494a384,
	0x9a879fa0, 0x68ec1ca3, 0x7bbcef57, 0x89d76c54,
	0x5d1d08bf, 0xaf768bbc, 0xbc267848, 0x4e4dfb4b,
	0x20bd8ede, 0xd2d60ddd, 0xc186fe29, 0x33ed7d2a,
	0xe72719c1, 0x154c9ac2, 0x061c6936, 0xf477ea35,
	0xaa64d611, 0x580f5512, 0x4b5fa6e6, 0xb93425e5,
	0x6dfe410e, 0x9f95c20d, 0x8cc531f9, 0x7eaeb2fa,
	0x30e349b1, 0xc288cab2, 0xd1d83946, 0x23b3ba45,
	0xf779deae, 0x05125dad, 0x1642ae59, 0xe4292d5a,
	0xba3a117e, 0x4851927d, 0x5b016189, 0xa96ae28a,
	0x7da08661, 0x8fcb0562, 0x9c9bf696, 0x6ef07595,
	0x417b1dbc, 0xb3109ebf, 0xa0406d4b, 0x522bee48,
	0x86e18aa3, 0x748a09a0, 0x67dafa54, 0x95b17957,
	0xcba24573, 0x39c9c670, 0x2a993584, 0xd8f2b687,
	0x0c38d26c, 0xfe53516f, 0xed03a29b, 0x1f682198,
	0x5125dad3, 0xa34e59d0, 0xb01eaa24, 0x42752927,
	0x96bf4dcc, 0x64d4cecf, 0x77843d3b, 0x85efbe38,
	0xdbfc821c, 0x2997011f, 0x3ac7f2eb, 0xc8ac71e8,
	0x1c661503, 0xee0d9600, 0xfd5d65f4, 0x0f36e6f7,
	0x61c69362, 0x93ad1061, 0x80fde395, 0x72966096,
	0xa65c047d, 0x5437877e, 0x4767748a, 0xb50cf789,
	0xeb1fcbad, 0x197448ae, 0x0a24bb5a, 0xf84f3859,
	0x2c855cb2, 0xdeeedfb1, 0xcdbe2c45, 0x3fd5af46,
	0x7198540d, 0x83f3d70e, 0x90a324fa, 0x62c8a7f9,
	0xb602c312, 0x44694011, 0x5739b3e5, 0xa55230e6,
	0xfb410cc2, 0x092a8fc1, 0x1a7a7c35, 0xe811ff36,
	0x3cdb9bdd, 0xceb018de, 0xdde0eb2a, 0x2f8b6829,
	0x82f63b78, 0x709db87b, 0x63cd4b8f, 0x91a6c88c,
	0x456cac67, 0xb7072f64, 0xa457dc90, 0x563c5f93,
	0x082f63b7, 0xfa44e0b4, 0xe9141340, 0x1b7f9043,
	0xcfb5f4a8, 0x3dde77ab, 0x2e8e845f, 0xdce5075c,
	0x92a8fc17, 0x60c37f14, 0x73938ce0, 0x81f80fe3,
	0x55326b08, 0xa759e80b, 0xb4091bff, 0x466298fc,
	0x1871a4d8, 0xea1a27db, 0xf94ad42f, 0x0b21572c,
	0xdfeb33c7, 0x2d80b0c4, 0x3ed04330, 0xccbbc033,
	0xa24bb5a6, 0x502036a5, 0x4370c551, 0xb11b4652,
	0x65d122b9, 0x97baa1ba, 0x84ea524e, 0x7681d14d,
	0x2892ed69, 0xdaf96e6a, 0xc9a99d9e, 0x3bc21e9d,
	0xef087a76, 0x1d63f975, 0x0e330a81, 0xfc588982,
	0xb21572c9, 0x407ef1ca, 0x532e023e, 0xa145813d,
	0x758fe5d6, 0x87e466d5, 0x94b49521, 0x66df1622,
	0x38cc2a06, 0xcaa7a905, 0xd9f75af1, 0x2b9cd9f2,
	0xff56bd19, 0x0d3d3e1a, 0x1e6dcdee, 0xec064eed,
	0xc38d26c4, 0x31e6a5c7, 0x22b65633, 0xd0ddd530,
	0x0417b1db, 0xf67c32d8, 0xe52cc12c, 0x1747422f,
	0x49547e0b, 0xbb3ffd08, 0xa86f0efc, 0x5a048dff,
	0x8ecee914, 0x7ca56a17, 0x6ff599e3, 0x9d9e1ae0,
	0xd3d3e1ab, 0x21b862a8, 0x32e8915c, 0xc083125f,
	0x144976b4, 0xe622f5b7, 0xf5720643, 0x07198540,
	0x590ab964, 0xab613a67, 0xb831c993, 0x4a5a4a90,
	0x9e902e7b, 0x6cfbad78, 0x7fab5e8c, 0x8dc0dd8f,
	0xe330a81a, 0x115b2b19, 0x020bd8ed, 0xf0605bee,
	0x24aa3f05, 0xd6c1bc06, 0xc5914ff2, 0x37faccf1,
	0x69e9f0d5, 0x9b8273d6, 0x88d28022, 0x7ab90321,
	0xae7367ca, 0x5c18e4c9, 0x4f48173d, 0xbd23943e,
	0xf36e6f75, 0x0105ec76, 0x12551f82, 0xe03e9c81,
	0x34f4f86a, 0xc69f7b69, 0xd5cf889d, 0x27a40b9e,
	0x79b737ba, 0x8bdcb4b9, 0x988c474d, 0x6ae7c44e,
	0xbe2da0a5, 0x4c4623a6, 0x5f16d052, 0xad7d5351,
};

uint32_t crc32_sctp(const void *buf, size_t size)
{
  auto *p = (const uint8_t*)buf;
  uint32_t crc = ~0U;

  if (size < k_sctp_common_header_size) {
    anon_log("too small to be a sctp header");
    return crc;
  }

  // up to the checksum field
  for (auto i = 0; i < 8; i++) {
    crc = crc32_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  }

  // compute as if the checksum were all zeros
  for (auto i = 0; i < 4; i++) {
    crc = crc32_tab[(crc ^ 0) & 0xff] ^ (crc >> 8);
  }

  // now compute the rest
  size -= k_sctp_common_header_size;
  p += 4;
  while (size--) {
    crc = crc32_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  }

  crc = ~crc;
  crc = (crc << 24)
    + ((crc << 8) & 0x00ff0000)
    + ((crc >> 8) & 0x0000ff00)
    + (crc >> 24);

  return crc;
}

}

bool sctp_dispatch::do_chunk_init(const uint8_t* init_data, ssize_t init_len)
{
  #if 0
  // RFC 4960, section 3.3.2
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |   Type = 1    |  Chunk Flags  |      Chunk Length             |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                         Initiate Tag                          |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |           Advertised Receiver Window Credit (a_rwnd)          |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |  Number of Outbound Streams   |  Number of Inbound Streams    |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                          Initial TSN                          |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  \                                                               \
  /              Optional/Variable-Length Parameters              /
  \                                                               \
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  #endif

  if (init_len < k_init_chunk_header_size) {
    anon_log("chunk length too small for CHNK_INIT");
    return false;
  }

  auto init_end = init_data + init_len;
  verification_tag = get_be_uint32(&init_data[4]);
  auto window_credit = get_be_uint32(&init_data[8]);
  auto out_streams = get_be_uint16(&init_data[12]);
  auto in_streams = get_be_uint16(&init_data[14]);
  last_complete_tsn = get_be_uint32(&init_data[16]) - 1;
  auto opt_start = &init_data[k_init_chunk_header_size];

  while (opt_start + k_sctp_option_header_size < init_end) {
    auto opt_type = opt_start[0];
    auto opt_flags = opt_start[1];
    auto opt_len = get_be_uint16(&opt_start[2]);
    if (opt_len < k_sctp_option_header_size) {
      anon_log("option length too small");
      return false;
    }
    if (opt_start + opt_len > init_end) {
      anon_log("option length too big");
      return false;
    }
    switch(opt_type) {
      case OPT_FORWARD_TSN:
        // good to know, no action
        break;
      case OPT_SUPPORTED_EXTENSIONS_FIRST_BYTE:
        if (opt_flags != OPT_SUPPORTED_EXTENSIONS_SECOND_BYTE) {
          anon_log("0x80 only permitted if second byte is 0x08");
          return false;
        }
        break;
      default:
        anon_log("unknown option type: " << (int)opt_type);
        return false;
    }
    opt_start += (opt_len + 3) & ~3;
  }

  // the INIT_ACK is going to be a copy of the INIT we recieved with
  // some modifications:
  //  1) we are required to return a cookie, but we don't use one
  //     internally, so we return an empty one.
  //  2) the chunk type is set to CHNK_INIT_ACK instead of CHNK_INIT

  auto cookie_len = k_sctp_option_header_size;
  auto rounded_init_len = (init_len + 3) & ~3;
  auto len_with_cookie = rounded_init_len + cookie_len;
  std::vector<uint8_t> ack_chunk(len_with_cookie);
  memcpy(&ack_chunk[0], init_data, init_len);
  //set_be_uint32(verification_tag, &reply[4]);
  ack_chunk[0] = CHNK_INIT_ACK;
  set_be_uint16(len_with_cookie, &ack_chunk[2]);
  set_be_uint16(OPT_COOKIE, &ack_chunk[rounded_init_len]);
  set_be_uint16(cookie_len, &ack_chunk[rounded_init_len+2]);
  tsns.insert(last_complete_tsn + 1);
  chunks.push_back(std::string((char*)&ack_chunk[0], ack_chunk.size()));
  return true;
}

bool sctp_dispatch::parse_sctp_chunks(const uint8_t* msg, ssize_t len)
{
  std::ostringstream oss;

  auto msg_end = msg + len;
  auto chunk_start = msg + k_sctp_common_header_size;
  while (chunk_start <= msg_end - k_sctp_chunk_header_size) {
    auto chunk_type = chunk_start[0];
    auto chunk_flags = chunk_start[1];
    auto chunk_len = get_be_uint16(&chunk_start[2]);
    auto rounded_chunk_len = ((chunk_len + 3) & ~3);
    if (chunk_len < k_sctp_chunk_header_size) {
      anon_log("invalid sctp chunk length field: " << chunk_len);
      return false;
    }
    if (chunk_start + chunk_len <= msg_end) {
      switch (chunk_type) {
        case CHNK_DATA:
          dcd->recv_data_chunk(chunk_start, chunk_len);
          break;
        case CHNK_INIT:
          if (!do_chunk_init(chunk_start, chunk_len)) {
            verification_tag = 0;
            return false;
          }
          break;
        case CHNK_INIT_ACK:
          oss << "CHNK_INIT_ACK ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_SACK:
          oss << "CHNK_SACK ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_HEARTBEAT:
          {
            std::string reply_chunk((char*)&chunk_start[0], chunk_len);
            reply_chunk.data()[0] = CHNK_HEARTBEAT_ACK;
            chunks.push_back(std::move(reply_chunk));
          }
          break;
        case CHNK_HEARTBEAT_ACK:
          oss << "CHNK_HEARTBEAT_ACK ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_ABORT:
          oss << "CHNK_ABORT ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_SHUTDOWN:
          oss << "CHNK_SHUTDOWN ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_SHUTDOWN_ACK:
          oss << "CHNK_SHUTDOWN_ACK ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_ERROR:
          oss << "CHNK_ERROR ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_COOKIE_ECHO:
          {
            std::vector<uint8_t> reply_chunk(4);
            reply_chunk[0] = CHNK_COOKIE_ACK;
            set_be_uint16(4, &reply_chunk[2]);
            chunks.push_back(std::string((char*)&reply_chunk[0], 4));
          }
          break;
        case CHNK_COOKIE_ACK:
          oss << "CHNK_COOKIE_ACK ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_ECNE:
          oss << "CHNK_ECNE ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_CWR:
          oss << "CHNK_CWR ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        case CHNK_SHUTDOWN_COMPLETE:
          oss << "CHNK_SHUTDOWN_COMPLETE ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
        default:
          oss << "unknown chunk type: " << std::hex << (int)chunk_type << std::dec << " ";
          append_bytes(&chunk_start[k_sctp_chunk_header_size], chunk_len-k_sctp_chunk_header_size, oss);
          anon_log(oss.str());
          oss.clear();
          break;
      }
    }
    else {
      anon_log("sctp msg only contains partial chunk");
      return false;
    }
    chunk_start += rounded_chunk_len;
  }
  return true;
}

void sctp_dispatch::send_acks()
{
  #if 0
  // RFC 4960, section 3.3.4 - SACK
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |   Type = 3    |Chunk  Flags   |      Chunk Length             |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                      Cumulative TSN Ack                       |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |          Advertised Receiver Window Credit (a_rwnd)           |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  | Number of Gap Ack Blocks = N  |  Number of Duplicate TSNs = X |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |  Gap Ack Block #1 Start       |   Gap Ack Block #1 End        |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  /                                                               /
  \                              ...                              \
  /                                                               /
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |   Gap Ack Block #N Start      |  Gap Ack Block #N End         |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                       Duplicate TSN 1                         |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  /                                                               /
  \                              ...                              \
  /                                                               /
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                       Duplicate TSN X                         |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  #endif

  // move last_complete_tsn forward to account for
  // all sequential tsns that we have received -
  // and remove those from 'tsns'
  for (auto it = tsns.begin(); it != tsns.end();) {
    if (*it == last_complete_tsn + 1) {
      ++last_complete_tsn;
      it = tsns.erase(it);
    }
    else {
      break;
    }
  }
  // last_complete_tsn is now "Cumulative TSN Ack"

  // now count and record the discontinuous tsns
  // that we have already received after that
  // are after last_complete_tsn
  std::vector<std::pair<uint32_t, uint32_t>> tsn_blocks;
  if (tsns.size() > 0) {
    auto prev = *tsns.begin();
    auto block_start = prev;
    prev -= 1;
    for (auto t : tsns) {
      if (t != prev+1) {
        tsn_blocks.push_back(std::make_pair(block_start-last_complete_tsn, prev-last_complete_tsn));
        block_start = t;
      }
      prev = t;
    }
    tsn_blocks.push_back(std::make_pair(block_start-last_complete_tsn, prev-last_complete_tsn));
  }

  // the size of our SACK chunk
  auto blocks_size = tsn_blocks.size() * 4;
  auto dups_size = duplicate_tsns.size() * 4;
  auto sack_size = k_sack_chunk_header_size + blocks_size + dups_size;

  std::vector<uint8_t> sack(sack_size);
  sack[0] = CHNK_SACK;
  set_be_uint16(sack_size, &sack[2]);
  set_be_uint32(last_complete_tsn, &sack[4]);
  set_be_uint32(65536*4, &sack[8]);
  set_be_uint16(tsn_blocks.size(), &sack[12]);
  set_be_uint16(duplicate_tsns.size(), &sack[14]);
  auto ptr = &sack[k_sack_chunk_header_size];
  for (auto &b : tsn_blocks) {
    set_be_uint16(b.first, &ptr[0]);
    set_be_uint16(b.second, &ptr[2]);
    ptr += 4;
  }
  for (auto dup : duplicate_tsns) {
    set_be_uint16(dup, ptr);
    ptr += 4;
  }


  #if 0
  // RRC 4960, section 3.1 - Common Header
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |     Source Port Number        |     Destination Port Number   |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                      Verification Tag                         |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                           Checksum                            |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  #endif

  size_t total_reply_size = k_sctp_common_header_size;
  for (auto &c : chunks) {
    auto sz = c.size();
    sz = (sz + 3) & ~3;
    total_reply_size += sz;
  }
  total_reply_size += sack.size();

  std::vector<uint8_t> reply(total_reply_size);
  set_be_uint16(local_port, &reply[0]);
  set_be_uint16(remote_port, &reply[2]);
  set_be_uint32(verification_tag, &reply[4]);
  ptr = &reply[k_sctp_common_header_size];
  for (auto &c : chunks) {
    auto sz = c.size();
    memcpy(ptr, c.c_str(), sz);
    sz = (sz + 3) & ~3;
    ptr += sz;
  }
  memcpy(ptr, &sack[0], sack.size());
  auto crc = crc32_sctp(&reply[0], reply.size());
  set_be_uint32(crc, &reply[8]);

  chunks.clear();
  duplicate_tsns.clear();

  send_reply(&reply[0], reply.size());
}

sctp_dispatch::sctp_dispatch(uint16_t local_port, uint16_t remote_port)
  : local_port(local_port),
    remote_port(remote_port)
{}

void sctp_dispatch::connect(std::function<void(const uint8_t* msg, size_t len)>&& send_rep)
{
  send_reply = std::move(send_rep);
  dcd = std::make_shared<data_channel_dispatch>(
    [wp = std::weak_ptr<sctp_dispatch>(shared_from_this())]
    (uint32_t tsn, const uint8_t* chunk, size_t len) {
      // add this chunk to the set of chunks we will return in response to
      // a call to our recv_msg, record which TSN's we are responding to
      if (auto ths = wp.lock()) {
        if (tsn > ths->last_complete_tsn) {
          auto it = ths->tsns.insert(tsn);
          if (!it.second) {
            ths->duplicate_tsns.push_back(tsn);
          }
        }
        if (len > 0) {
          ths->chunks.push_back(std::string((char*)chunk, len));
        }
      }
    });
}

// SCTP on top of (UDP) DTLS
// RFC 8261

void sctp_dispatch::recv_msg(const uint8_t *msg, ssize_t len)
{
  // std::ostringstream oss;
  // append_bytes(msg, len, oss);
  // anon_log("got sctp msg:\n" << oss.str());

  if (len >= k_sctp_common_header_size) {
    auto computed_crc = crc32_sctp(msg, len);
    auto provided_src = get_be_uint32(&msg[8]);
    if (computed_crc == provided_src) {
      if (parse_sctp_chunks(msg, len)) {
        send_acks();
      }
    } else {
      std::ostringstream oss;
      append_bytes(msg, len, oss);
      anon_log("ignoring sctp msg with crc mismatch, should be: " << computed_crc << ", but was provided as: " << provided_src << "\n" << oss.str());
    }
  }
}
