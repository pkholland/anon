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

#include "stun.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <openssl/hmac.h>
#include "log.h"
#include "webrtc_connection.pb.h"

namespace {

// table used by one of the standard polynomials for crc32.  This
// is the one used by stun's FINGERPRINT attribute
const uint32_t crc32_tab[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

// some constants
enum {
  stun_msg_header_size = 20,  // 16bit method, 16bit len, 32bit magic cookie, 96bit transaction id
  attribute_header_size = 4,  // 16bit type, 16bit len
  fingerprint_xor_value = 0x5354554E, // used to somewhat obfuscate the crc32 value

  method_class_mask = 0x0110,  // where the two "class" bits are in a 16bit method (after reading into correct endianness)
  request_class = 0x0000,
  indication_class = 0x0010,
  success_response_class = 0x0100,
  error_response_class = 0x0110
};

// stun methods we understand
enum {
  binding = 0x0001
};

// stun attributes we understand
enum {
  MAPPED_ADDRESS = 0x0001,
  USERNAME = 0x0006,
  MESSAGE_INTEGRITY = 0x0008,
  REALM = 0x0014,
  NONCE =  0x0015,
  XOR_MAPPED_ADDRESS = 0x0020,
  PRIORITY = 0x0024,
  USE_CANDIDATE = 0x0025,
  FINGERPRINT = 0x8028,
  ICE_CONTROLLED = 0x8029,
  ICE_CONTROLLING = 0x802A,
};

// this is the stun "magic cookie" 0x2112a442, but represented in native endian order
// when loaded from big-endian storage (as we expect it to be in the stun message itself).
uint32_t raw_magic_cookie;

// when an ipv4 address gets converted into an ipv6 address (for a "dualstack" socket)
// its first 12 bytes are this.  The last 4 are the ipv4 address itself.
std::vector<unsigned char> ipv4_in_6_header = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

// read and write various integer sizes to/from big-endian storage

uint16_t get_uint16(const uint8_t *ptr)
{
  uint16_t val = ptr[0];
  val <<= 8;
  return val + ptr[1];
}

void set_uint16(uint16_t val, uint8_t *ptr)
{
  ptr[0] = (val >> 8) & 0x00ff;
  ptr[1] = val & 0x00ff;
}

uint32_t get_uint32(const uint8_t *ptr)
{
  uint32_t val = ptr[0];
  val <<= 8;
  val += ptr[1];
  val <<= 8;
  val += ptr[2];
  val <<= 8;
  return val + ptr[3];
}

void set_uint32(uint32_t val, uint8_t *ptr)
{
  ptr[0] = (val >> 24) & 0x00ff;
  ptr[1] = (val >> 16) & 0x00ff;
  ptr[2] = (val >> 8) & 0x00ff;
  ptr[3] = val & 0x00ff;
}

uint64_t get_uint64(const uint8_t *ptr)
{
  uint64_t val = 0;
  for (auto i = 0; i < 8; i++) {
    val <<= 8;
    val += ptr[i];
  }
  return val;
}

void set_uint64(uint64_t val, uint8_t* ptr)
{
  for (auto i = 0; i < 8; i++) {
    ptr[i] = (val >> (56 - i*8)) & 0x0ff;
  }
}

// "standard" (defined by stun) crc32 value - followed by xoring the special value
uint32_t crc32_xor(const void *buf, size_t size)
{
  auto *p = (const uint8_t*)buf;
  uint32_t crc;

  crc = ~0U;
  while (size--) {
    crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  }
  return (crc ^ ~0U) ^ fingerprint_xor_value;
}

std::set<uint16_t> known_methods = {binding};
std::set<uint16_t> known_required_attribs = {
  MAPPED_ADDRESS,
  USERNAME,
  MESSAGE_INTEGRITY,
  XOR_MAPPED_ADDRESS,
  PRIORITY,
  USE_CANDIDATE
};

// some debugging helper stuff
std::map<uint16_t, std::string> attribute_names = {
  {0x0000, "(Reserved)"},
  {0x0001, "MAPPED-ADDRESS"},
  {0x0002, "(Reserved; was RESPONSE-ADDRESS)"},
  {0x0003, "(Reserved; was CHANGE-ADDRESS)"},
  {0x0004, "(Reserved; was SOURCE-ADDRESS)"},
  {0x0005, "(Reserved; was CHANGED-ADDRESS)"},
  {0x0006, "USERNAME"},
  {0x0007, "(Reserved; was PASSWORD)"},
  {0x0008, "MESSAGE-INTEGRITY"},
  {0x0009, "ERROR-CODE"},
  {0x000A, "UNKNOWN-ATTRIBUTES"},
  {0x000B, "(Reserved; was REFLECTED-FROM)"},
  {0x0014, "REALM"},
  {0x0015, "NONCE"},
  {0x0020, "XOR-MAPPED-ADDRESS"},
  {0x0021, "(Reserved; was TIMER-VAL)"},
  {0x0022, "RESERVATION-TOKEN	[RFC8656]"},
  {0x0023, "Reserved (0x0023)"},	
  {0x0024, "PRIORITY	[RFC8445]"},
  {0x0025, "USE-CANDIDATE"},
  {0x0026, "PADDING	[RFC5780]"},
  {0x0027, "RESPONSE-PORT	[RFC5780]"},
  {0x002A, "CONNECTION-ID	[RFC6062]"},
  {0x8000, "ADDITIONAL-ADDRESS-FAMILY	[RFC8656]"},
  {0x8001, "ADDRESS-ERROR-CODE	[RFC8656]"},
  {0x8002, "PASSWORD-ALGORITHMS	[RFC8489]"},
  {0x8003, "ALTERNATE-DOMAIN	[RFC8489]"},
  {0x8004, "ICMP	[RFC8656]"},
  {0x8022, "SOFTWARE	[RFC8489]"},
  {0x8023, "ALTERNATE-SERVER	[RFC8489]"},
  {0x8024, "Reserved (0x8024)"},
  {0x8025, "TRANSACTION_TRANSMIT_COUNTER	[RFC7982]"},
  {0x8026, "Reserved (0x8026)"},
  {0x8027, "CACHE-TIMEOUT [RFC5780]"},
  {0x8028, "FINGERPRINT	[RFC8489]"},
  {0x8029, "ICE-CONTROLLED	[RFC8445]"},
  {0x802A, "ICE-CONTROLLING	[RFC8445]"},
  {0x802B, "RESPONSE-ORIGIN	[RFC5780]"},
  {0x802C, "OTHER-ADDRESS	[RFC5780]"},
  {0x802D, "ECN-CHECK STUN	[RFC6679]"},
  {0x802E, "THIRD-PARTY-AUTHORIZATION	[RFC7635]"},
  {0x802F, "Unassigned (0x802F)"},
  {0x8030, "MOBILITY-TICKET	[RFC8016]"},
  {0xC000, "CISCO-STUN-FLOWDATA	[Dan_Wing]"},
  {0xC001, "ENF-FLOW-DESCRIPTION	[Pål_Erik_Martinsen]"},
  {0xC002, "ENF-NETWORK-STATUS	[Pål_Erik_Martinsen]"},
  {0xC057, "GOOG-NETWORK-INFO	[Jonas_Oreland]"},
  {0xC058, "GOOG-LAST-ICE-CHECK-RECEIVED	[Jonas_Oreland]"},
  {0xC059, "GOOG-MISC-INFO	[Jonas_Oreland]"},
  {0xC05A, "GOOG-OBSOLETE-1	[Jonas_Oreland]"},
  {0xC05B, "GOOG-CONNECTION-ID	[Jonas_Oreland]"},
  {0xC05C, "GOOG-DELTA	[Jonas_Oreland]"},
  {0xC05D, "GOOG-DELTA-ACK	[Jonas_Oreland]"},
  {0xC05E, "GOOG-DELTA-SYNC-REQ	[Jonas_Oreland]"},
  {0xC060, "GOOG-MESSAGE-INTEGRITY-32	[Jonas_Oreland]"}
};

struct stun_message_builder
{
  std::vector<uint8_t> buff;

  stun_message_builder(uint16_t message_type, const uint8_t* trans_id)
    : buff(20)
  {
    set_uint16(message_type, &buff[0]);
    // buf[2] and [3] will be the length
    *(uint32_t*)&buff[4] = raw_magic_cookie;
    memcpy(&buff[8], &trans_id[0], 12);
  }

  void add_ipv4_xor_mapped_address(uint16_t big_endian_port, const uint8_t* addr)
  {
    uint8_t attr[12] = {0};
    set_uint16(XOR_MAPPED_ADDRESS, &attr[0]);
    set_uint16(8/*len*/, &attr[2]);
    set_uint16(1/*familiey == ipv4*/, &attr[4]);
    *(uint16_t*)&attr[6] = big_endian_port ^ *(uint16_t*)&buff[4]/*first two bytes of magic_cookie*/;
    auto s1 = addr;
    auto s2 = &buff[4];
    auto d = &attr[8];
    for (auto i = 0; i < 4; i++) {
      *d++ = *s1++ ^ *s2++;
    }
    auto sz = (buff.size() + 3) & 0x0fffc;
    buff.resize(sz + 12);
    memcpy(&buff[sz], &attr[0], 12);
  }

  void add_xor_mapped_address(const sockaddr_storage *sockaddr)
  {
    if (sockaddr->ss_family == AF_INET6) {
      auto addr6 = (sockaddr_in6*)sockaddr;
      // check for ipv4 wrapped in ipv6
      auto addr = addr6->sin6_addr.s6_addr;
      if (!memcmp(addr, &ipv4_in_6_header[0], 12)) {
        add_ipv4_xor_mapped_address(addr6->sin6_port, &addr[12]);
      }
      else {
        uint8_t attr[24] = {0};
        set_uint16(XOR_MAPPED_ADDRESS, &attr[0]);
        set_uint16(20/*len*/, &attr[2]);
        set_uint16(1/*family == ipv6*/, &attr[4]);
        *(uint16_t*)&attr[6] = addr6->sin6_port ^ *(uint16_t*)&buff[4]/*first two bytes of magic_cookie*/;
        auto s1 = &addr[0];
        auto s2 = &buff[4];
        auto d = &attr[8];
        for (auto i = 0; i < 16; i++) {
          *d++ = *s1++ ^ *s2++;
        }
        auto sz = (buff.size() + 3) & 0x0fffc;
        buff.resize(sz + 24);
        memcpy(&buff[sz], &attr[0], 24);
      }
    }
    else {
      auto addr4 = (sockaddr_in*)sockaddr;
      add_ipv4_xor_mapped_address(addr4->sin_port, (uint8_t*)&addr4->sin_addr);
    }
  }

  void add_user_name(const std::string& user_name)
  {
    auto sz = (buff.size() + 3) & 0x0fffc;
    auto usz = (uint16_t)user_name.size();
    buff.resize(sz + 4 + usz);
    set_uint16(USERNAME, &buff[sz]);
    set_uint16(usz, &buff[sz+2]);
    memcpy(&buff[4], user_name.c_str(), usz);
  }

  void add_ice_controlled()
  {
    auto sz = (buff.size() + 3) & 0x0fffc;
    buff.resize(sz + 12);
    set_uint16(ICE_CONTROLLED, &buff[sz]);
    set_uint16(8, &buff[sz+2]);
    set_uint64(0, &buff[sz+4]);
  }

  void add_priority(uint32_t priority)
  {
    auto sz = (buff.size() + 3) & 0x0fffc;
    buff.resize(sz + 8);
    set_uint16(PRIORITY, &buff[sz]);
    set_uint16(4, &buff[sz+2]);
    set_uint32(priority, &buff[sz+4]);
  }

  void add_message_integrity(const std::string& pwd)
  {
    auto sz = (buff.size() + 3) &0x0fffc;
    buff.resize(sz + 24);
    set_uint16(sz + 24 - stun_msg_header_size, &buff[2]);
    set_uint16(MESSAGE_INTEGRITY, &buff[sz]);
    set_uint16(20, &buff[sz+2]);
    unsigned int attr_len = 20;
    HMAC(EVP_sha1(), pwd.c_str(), pwd.size(), &buff[0], sz, &buff[sz+4], &attr_len);
  }

  void add_fingerprint()
  {
    auto sz = (buff.size() + 3) & 0x0fffc;
    buff.resize(sz + 8);
    set_uint16(sz + 8 - stun_msg_header_size, &buff[2]);
    set_uint16(FINGERPRINT, &buff[sz]);
    set_uint16(4, &buff[sz+2]);
    set_uint32(crc32_xor(&buff[0], sz), &buff[sz+4]);
  }
};


}

stun_msg_parser::stun_msg_parser(std::function<std::shared_ptr<std::string>(const std::string& username)> lookup)
  : lookup(std::move(lookup))
{
  auto ptr = (uint8_t*)&raw_magic_cookie;
  ptr[0] = 0x21;
  ptr[1] = 0x12;
  ptr[2] = 0xa4;
  ptr[3] = 0x42;
}

stun_msg_parser::stun_msg stun_msg_parser::parse_stun_msg(const unsigned char *msg, ssize_t len)
{
  std::string user_name;
  webrtc::Connection conn;
  auto has_fingerprint = false;
  auto has_use_candidate = false;
  auto has_ice_controlling = false;
  auto known_client = false;
  uint16_t method{0};
  uint16_t method_class{0};

  // trivial "is this stun" tests
  if (len < 20 || (msg[0] & 0xc0) != 0 || *(uint32_t*)&msg[4] != raw_magic_cookie) {
    if (len < 20) {
      anon_log("message too short");
    }
    else if ((msg[0] & 0xc0) != 0) {
      anon_log("first two bits not zero");
    }
    else {
      anon_log("magic cookie incorrect");
    }
    return {};
  }

  auto msgSize = get_uint16(&msg[2]);
  if (len != msgSize + stun_msg_header_size) {
    anon_log("message size mismatch.  len: " << len << ", msgSize+stun_msg_header_size: " << msgSize+stun_msg_header_size);
    return {};
  }

  auto mth = get_uint16(&msg[0]);
  method = mth & ~method_class_mask;
  method_class = mth & method_class_mask;
  if (known_methods.find(method) == known_methods.end()) {
    anon_log("unknown method: " << method);
    return {};
  }

  auto msg_end = &msg[len];
  auto ptr = &msg[stun_msg_header_size];
  while (ptr < msg_end) {
    if (ptr + attribute_header_size > msg_end) {
      anon_log("invalid attribute data");
      return {};
    }
    auto attr_type = get_uint16(&ptr[0]);
    auto attr_len = get_uint16(&ptr[2]);
    if (ptr + attribute_header_size + attr_len > msg_end) {
      anon_log("next attribute value past end");
      return {};
    }
    #if 0
    {
      auto nm = attribute_names.find(attr_type);
      if (nm != attribute_names.end()) {
        anon_log(nm->second << ", len: " << attr_len);
      }
      else {
        anon_log("unknown attribute (" << attr_type << "), len: " << attr_len);
      }
    }
    #endif
    if (!(attr_type & 0x8000) && known_required_attribs.find(attr_type) == known_required_attribs.end()) {
      auto nm = attribute_names.find(attr_type);
      if (nm != attribute_names.end()) {
        anon_log("unimplemented, required attribute: " << nm->second);
      }
      else {
        anon_log("unknown, required attribute: " << attr_type);
      }
      return {};
    }
    switch(attr_type) {
      case USERNAME: {
        auto pos = memchr(&ptr[attribute_header_size], ':', attr_len);
        if (!pos) {
          anon_log("username contains no \":\" chr");
          return {};
        }
        if (pos == &ptr[attribute_header_size] || pos == &ptr[attribute_header_size+attr_len]) {
          anon_log("empty ufrag");
          return {};
        }
        user_name = std::string((const char*)&ptr[attribute_header_size], attr_len);
        auto res = lookup(user_name);
        if (!res) {
          anon_log("no rtc connection registered for: " << user_name);
          return {};
        }
        if (!conn.ParseFromString(*res)) {
          anon_log("unable to parse connection data");
          return {};
        }
        known_client = true;
      } break;
      case MESSAGE_INTEGRITY:
        if (attr_len != 20) {
          anon_log("wrong message integrity length");
          return {};
        }
        else if (user_name.empty()) {
          anon_log("MESSAGE_INTEGRITY without USERNAME");
          return {};
        }
        else {
          // make a copy of msg, up to the start of this MESSAGE_INTEGRITY
          // attribute, but with the stun header size set to the size we would
          // get if the whole message ended at the end of this MESSAGE_INTEGRITY
          // attribute.  When the creator of this message computed the HMAC
          // they had already incremented this length field, but they only
          // use the bytes up to the start of this integrity field - they
          // don't include the bytes of the integetrity field itself (obviously,
          // since they are in the process of computing those bytes).
          auto end_ptr = ptr + attribute_header_size + attr_len;
          auto modified_len = end_ptr - (msg + stun_msg_header_size);
          auto cpy_size = ptr - msg;
          std::vector<uint8_t> cpy(cpy_size);
          memcpy(&cpy[0], msg, cpy_size);
          set_uint16(modified_len, &cpy[2]);
          auto &pwd = conn.local_pwd();

          uint8_t md[20];
          unsigned int md_len = sizeof(md);
          HMAC(EVP_sha1(), pwd.c_str(), pwd.size(), &cpy[0], cpy.size(), &md[0], &md_len);
          if (memcmp(&md[0], &ptr[attribute_header_size], 20)) {
            anon_log("message integrity mismatch");
            return {};
          }
        }
        break;
      case USE_CANDIDATE:
        if (attr_len != 0) {
          anon_log("wrong USE-CANDIDATE length");
          return {};
        }
        has_use_candidate = true;
        break;
      case ICE_CONTROLLING:
        if (attr_len != 8) {
          anon_log("wrong ICE-CONGTROLLING length");
          return {};
        }
        has_ice_controlling = true;
        break;
      case FINGERPRINT:
        if (attr_len != 4) {
          anon_log("wrong fingerprint length");
          return {};
        } else {
          auto presented_crc = get_uint32(&ptr[attribute_header_size]);
          auto computed_crc = crc32_xor(msg, ptr - msg);
          if (computed_crc != presented_crc) {
            anon_log("fingerprint mismatch");
            return {};
          }
        }
        break;
    }
    ptr += attribute_header_size + ((attr_len + 3) & 0x0fffc);
  }

  return stun_msg(
    method,
    method_class,
    conn.remote_ufrag(),
    conn.remote_pwd(),
    conn.local_ufrag(),
    conn.local_pwd(),
    has_fingerprint,
    has_use_candidate,
    has_ice_controlling,
    known_client
  );
}

std::vector<uint8_t> stun_msg_parser::create_stun_reply(stun_msg&& stun, const uint8_t* msg,
  const struct sockaddr_storage *sockaddr)
{
  if (stun.method_class == request_class) {
    if (stun.method == binding) {
      stun_message_builder response(binding | success_response_class, &msg[8]);
      response.add_xor_mapped_address(sockaddr);
      if (stun.has_ice_controlling) {
        response.add_ice_controlled();
      }
      if (!stun.local_pwd.empty()) {
        response.add_message_integrity(stun.local_pwd);
      }
      response.add_fingerprint();
      return response.buff;
    }
  }
  return {};
}
