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

#include "mcdc.h"
#include "dns_lookup.h"

mcd_cluster::mcd_cluster(const char* host, int port,
                        int max_conn_per_ep,
                        int lookup_frequency_in_seconds)
  : host_(host),
    port_(port),
    clstr_([this]()->std::pair<int, std::vector<std::pair<int, sockaddr_in6>>>
          {
            auto rslt = dns_lookup::get_addrinfo(host_.c_str(), port_);
            if (rslt.first)
              return std::make_pair(rslt.first, std::vector<std::pair<int, sockaddr_in6>>());
            std::vector<std::pair<int, sockaddr_in6>> addrs;
            for (auto i = 0; i < rslt.second.size(); i++)
              addrs.push_back(std::make_pair(0,rslt.second[i]));
            return std::make_pair(0, addrs);
          },
          false,  // do_tls - memcached never runs over tls
          "",     // host_name_for_tls
          0,      // tls_context
          max_conn_per_ep,
          lookup_frequency_in_seconds)
{}

void mcd_cluster::set(const std::string& key, const std::string& val, int expiration, int flags, int vbucket)
{
  auto body_size = 8/*extra*/ + key.size() + val.size();
  auto pkt_size = 24/*header*/ + body_size;
  std::vector<unsigned char> pkt(pkt_size);
  
  pkt[0] = 0x80;  // magic byte for "request"
  pkt[1] = 0x01;  // opcode for "set"
  *((uint16_t*)&pkt[2]) = htons(key.size());
  pkt[4] = 0x08;  // size of extra data
  pkt[5] = 0x00;  // data type - "raw bytes"
  *((uint16_t*)&pkt[6]) = htons(vbucket);
  *((uint32_t*)&pkt[8]) = htonl(body_size);
  memset(&pkt[12], 0, 12);  // 4 bytes of "opaque" followed by 8 bytes of "data version check"
  *((uint32_t*)&pkt[24]) = htonl(flags);
  *((uint32_t*)&pkt[28]) = htonl(expiration);
  memcpy(&pkt[32], key.c_str(), key.size());
  memcpy(&pkt[32 + key.size()], val.c_str(), val.size());
  
  std::vector<unsigned char> reply(24);
  clstr_.with_connected_pipe([&pkt, &reply](const pipe_t* pipe)
  {
    pipe->write(&pkt[0], pkt.size());
    int bytes_read = 0;
    while (bytes_read < 24)
      bytes_read += pipe->read(&reply[bytes_read], 24 - bytes_read);
    auto body_size = ntohl(*((uint32_t*)&reply[8]));
    if (body_size > 0) {
      reply.resize(24 + body_size);
      while (bytes_read < 24 + body_size)
        bytes_read += pipe->read(&reply[bytes_read], 24 + body_size - bytes_read);
    }

    if (reply[0] != 0x81)
      do_error("invalid memcached reply, magic byte was: " << (int)reply[0] << " instead of 129");
    status_check(ntohs(*((uint16_t*)&reply[6])));

    if (reply[1] != 0x01)
      do_error("invalid memcached reply to \"set\", opcode was: " << (int)reply[1] << " instead of 1");
    if (ntohs(*((uint16_t*)&reply[2])) != 0)
      do_error("invalid memcached reply to \"set\", contained a key_length of " << (int)ntohs(*((uint16_t*)&reply[2])));
    if (reply[4] != 0x00)
      do_error("invalid memcached reply to \"set\", contained a extra data of length: " << (int)reply[4]);
    if (reply[5] != 0x00)
      do_error("invalid memcached reply, specified data type: " << (int)reply[5]);
    if (ntohl(*((uint32_t*)&reply[8])) != 0)
      do_error("invalid memcached reply to \"set\", contained body length of: " << (int)*((uint32_t*)&reply[8]));
  });
  
}

std::string mcd_cluster::get(const std::string& key, int vbucket)
{
  auto body_size = key.size();
  auto pkt_size = 24/*header*/ + body_size;
  std::vector<unsigned char> pkt(pkt_size);

  pkt[0] = 0x80;  // magic byte for "request"
  pkt[1] = 0x00;  // opcode for "get"
  *((uint16_t*)&pkt[2]) = htons(key.size());
  pkt[4] = 0x00;  // size of extra data
  pkt[5] = 0x00;  // data type - "raw bytes"
  *((uint16_t*)&pkt[6]) = htons(vbucket);
  *((uint32_t*)&pkt[8]) = htonl(body_size);
  memset(&pkt[12], 0, 12);  // 4 bytes of "opaque" followed by 8 bytes of "data version check"
  memcpy(&pkt[24], key.c_str(), key.size());

  std::vector<unsigned char> reply(24);
  std::string ret;
  clstr_.with_connected_pipe([&pkt, &reply, &ret](const pipe_t* pipe)
  {
    pipe->write(&pkt[0], pkt.size());
    int bytes_read = 0;
    while (bytes_read < 24)
      bytes_read += pipe->read(&reply[bytes_read], 24 - bytes_read);
    auto body_size = ntohl(*((uint32_t*)&reply[8]));
    if (body_size > 0) {
      reply.resize(24 + body_size);
      while (bytes_read < 24 + body_size)
        bytes_read += pipe->read(&reply[bytes_read], 24 + body_size - bytes_read);
    }

    if (reply[0] != 0x81)
      do_error("invalid memcached reply, magic byte was: " << (int)reply[0] << " instead of 129");
    status_check(ntohs(*((uint16_t*)&reply[6])));

    if (reply[1] != 0x00)
      do_error("invalid memcached reply to \"get\", opcode was: " << (int)reply[1] << " instead of 0");
    if (reply[4] != 0x04)
      do_error("invalid memcached reply to \"get\", contained a extra data of length: " << (int)reply[4] << " instead of 4");
    if (reply[5] != 0x00)
      do_error("invalid memcached reply, specified data type: " << (int)reply[5]);
    
    auto bl = ntohl(*((uint32_t*)&reply[8]));
    ret = std::string((char*)&reply[28], bl-4/*extra data size*/);
  });
 
  return ret;
}


void mcd_cluster::status_check(uint16_t status)
{
  const char* err;
  switch (status) {
    case 0: // good to go
      return;
    case 0x0001:
      err = "Key not found";
      break;
    case 0x0002:
      err = "Key exists";
      break;
    case 0x0003:
      err = "Value too large";
      break;
    case 0x0004:
      err = "Invalid arguments";
      break;
    case 0x0005:
      err = "Item not stored";
      break;
    case 0x0006:
      err = "Incr/Decr on non-numeric value.";
      break;
    case 0x0007:
      err = "The vbucket belongs to another server";
      break;
    case 0x0008:
      err = "Authentication error";
      break;
    case 0x0009:
      err = "Authentication continue";
      break;
    case 0x0081:
      err = "Unknown command";
      break;
    case 0x0082:
      err = "Out of memory";
      break;
    case 0x0083:
      err = "Not supported";
      break;
    case 0x0084:
      err = "Internal error";
      break;
    case 0x0085:
      throw fiber_io_error("Busy", 4/*back off seconds*/, true/*close_socket_hint*/);
    case 0x0086:
      err = "Temporary failure";
      throw fiber_io_error("Temporary failure", 4/*back off seconds*/, true/*close_socket_hint*/);
    default:
      err = "unknown status";
      break;
  }
  
  std::ostringstream  oss;
  oss << "memcached error: " << err;
  errno = 0;
  throw std::runtime_error(oss.str());
}


