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

#include "dtls_bio.h"
#include "tls_context.h"
#include "fiber.h"
#include "tcp_utils.h"
#include <deque>
#include <openssl/opensslv.h>

///////////////////////////////////////////////////////////////

namespace
{

class simple_queue_io
{
public:
  simple_queue_io(const sockaddr_storage& addr)
    : addr_(addr),
      addr_len_(addr.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6))
  {}

  ~simple_queue_io() = default;

  std::deque<std::vector<char>> buffs_;
  int addr_len_;
  sockaddr_storage addr_;
};

class upd_sock_io
{
public:
  upd_sock_io(int sock, const sockaddr_storage& addr)
    : sock_(sock),
      addr_(addr),
      addr_len_(addr.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6))
  {}

  ~upd_sock_io() = default;

  int sock_;
  int addr_len_;
  sockaddr_storage addr_;
};


int simple_queue_new(BIO *b)
{
  BIO_set_init(b, 1);
  BIO_set_data(b, 0);
  return 1;
}

int simple_queue_free(BIO *b)
{
  if (b == NULL)
    return 0;
  auto p = reinterpret_cast<simple_queue_io *>(BIO_get_data(b));
  if (p)
    delete p;
  BIO_set_data(b, 0);
  return 1;
}

int simple_queue_read(BIO *b, char *out, int outl)
{
  auto ioq = reinterpret_cast<simple_queue_io *>(BIO_get_data(b));
  if (ioq)
  {
    if (ioq->buffs_.empty()) {
      return -1;
    }
    auto& f = ioq->buffs_.front();
    auto sz = f.size();
    if (sz > outl) {
      anon_log("only returing partial data");
      sz = outl;
    }
    memcpy(out, &f[0], sz);
    ioq->buffs_.pop_front();
    return sz;
  }
  return -1;
}

int simple_queue_write(BIO *b, const char *in, int inl)
{
  auto ioq = reinterpret_cast<simple_queue_io *>(BIO_get_data(b));
  if (ioq)
  {
    std::vector<char> b(inl);
    memcpy(&b[0], in, inl);
    ioq->buffs_.push_back(std::move(b));
    return inl;
  }
  return -1;
}

long simple_queue_ctrl(BIO *b, int cmd, long num, void *ptr)
{
  long ret = 1;
  auto ioq = reinterpret_cast<simple_queue_io *>(BIO_get_data(b));

  switch (cmd)
  {
  case BIO_CTRL_RESET:
    anon_log("ioq_ctrl BIO_CTRL_RESET");
    break;
  case BIO_CTRL_EOF:
    //anon_log("ioq_ctrl BIO_CTRL_EOF");
    break;
  case BIO_CTRL_INFO:
    anon_log("ioq_ctrl BIO_CTRL_INFO");
    break;
  case BIO_CTRL_SET:
    anon_log("ioq_ctrl BIO_CTRL_SET");
    break;
  case BIO_CTRL_GET:
    anon_log("ioq_ctrl BIO_CTRL_GET");
    break;
  case BIO_CTRL_PUSH:
    //anon_log("ioq_ctrl BIO_CTRL_PUSH");
    break;
  case BIO_CTRL_POP:
    //anon_log("ioq_ctrl BIO_CTRL_POP");
    break;
  case BIO_CTRL_GET_CLOSE:
    anon_log("ioq_ctrl BIO_CTRL_GET_CLOSE");
    break;
  case BIO_CTRL_SET_CLOSE:
    anon_log("ioq_ctrl BIO_CTRL_SET_CLOSE");
    break;
  case BIO_CTRL_PENDING:
    anon_log("ioq_ctrl BIO_CTRL_PENDING");
    ret = 0;
    break;
  case BIO_CTRL_FLUSH:
    //anon_log("ioq_ctrl BIO_CTRL_FLUSH");
    break;
  case BIO_CTRL_DUP:
    anon_log("ioq_ctrl BIO_CTRL_DUP");
    break;
  case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
    // anon_log("ioq_ctrl BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT");
    break;
  case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD:
    ret = 28;
    break;
  case BIO_CTRL_DGRAM_QUERY_MTU:
    ret = 1400;
    break;
  case BIO_CTRL_DGRAM_SET_MTU:
    ret = num;
    break;
  case BIO_CTRL_GET_KTLS_SEND:  // TODO, investiage ktls support
    ret = 0;
    break;
  case BIO_CTRL_GET_KTLS_RECV:
    ret = 0;
    break;
  case BIO_CTRL_WPENDING:
    // anon_log("ioq_ctrl BIO_CTRL_WPENDING");
    break;
  case BIO_CTRL_DGRAM_SET_PEER:
    memcpy(&ioq->addr_, ptr, ioq->addr_len_);
    break;
  case BIO_CTRL_DGRAM_GET_PEER:
    memcpy(ptr, &ioq->addr_, ioq->addr_len_);
    ret = ioq->addr_len_;
    break;
  default:
    anon_log("ioq_ctrl unknown: " << cmd);
    ret = 0;
    break;
  }
  return ret;
}

int simple_queue_puts(BIO *b, const char *str)
{
  return simple_queue_write(b, str, strlen(str));
}

int simple_queue_gets(BIO *b, char *buf, int size)
{
  int ret = 0;
  char *ptr = buf;
  char *end = buf + size - 1;

  while ((ptr < end) && (simple_queue_read(b, ptr, 1) > 0) && (ptr[0] != '\n'))
    ptr++;

  ptr[0] = 0;
  return strlen(buf);
}


////////////////////////////////////////////////////////////////

// when an ipv4 address gets converted into an ipv6 address (for a "dualstack" socket)
// its first 12 bytes are this.  The last 4 are the ipv4 address itself.
std::vector<unsigned char> ipv4_in_6_header = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

int udp_sock_free(BIO *b)
{
  if (b == NULL)
    return 0;
  auto p = reinterpret_cast<upd_sock_io *>(BIO_get_data(b));
  if (p)
    delete p;
  BIO_set_data(b, 0);
  return 1;
}

int udp_sock_read(BIO *b, char *out, int outl)
{
  anon_log("attempting to read from a (write-only) udp_sock BIO");
  return -1;
}

int udp_sock_write(BIO *b, const char *in, int inl)
{
  auto ioq = reinterpret_cast<upd_sock_io *>(BIO_get_data(b));
  if (ioq)
  {
    return ::sendto(ioq->sock_, in, inl, 0, (const sockaddr*)&ioq->addr_, ioq->addr_len_);
  }
  return -1;
}

long udp_sock_ctrl(BIO *b, int cmd, long num, void *ptr)
{
  long ret = 1;
  auto ioq = reinterpret_cast<upd_sock_io *>(BIO_get_data(b));

  switch (cmd)
  {
  case BIO_CTRL_RESET:
    anon_log("udp_sock_ctrl BIO_CTRL_RESET");
    break;
  case BIO_CTRL_EOF:
    //anon_log("udp_sock_ctrl BIO_CTRL_EOF");
    break;
  case BIO_CTRL_INFO:
    anon_log("udp_sock_ctrl BIO_CTRL_INFO");
    break;
  case BIO_CTRL_SET:
    anon_log("udp_sock_ctrl BIO_CTRL_SET");
    break;
  case BIO_CTRL_GET:
    anon_log("udp_sock_ctrl BIO_CTRL_GET");
    break;
  case BIO_CTRL_PUSH:
    //anon_log("udp_sock_ctrl BIO_CTRL_PUSH");
    break;
  case BIO_CTRL_POP:
    //anon_log("udp_sock_ctrl BIO_CTRL_POP");
    break;
  case BIO_CTRL_GET_CLOSE:
    anon_log("udp_sock_ctrl BIO_CTRL_GET_CLOSE");
    break;
  case BIO_CTRL_SET_CLOSE:
    anon_log("udp_sock_ctrl BIO_CTRL_SET_CLOSE");
    break;
  case BIO_CTRL_PENDING:
    anon_log("udp_sock_ctrl BIO_CTRL_PENDING");
    ret = 0;
    break;
  case BIO_CTRL_FLUSH:
    //anon_log("udp_sock_ctrl BIO_CTRL_FLUSH");
    break;
  case BIO_CTRL_DUP:
    anon_log("udp_sock_ctrl BIO_CTRL_DUP");
    break;
  case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
    // anon_log("udp_sock_ctrl BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT");
    break;
  case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD:
    ret = 28;
    break;
  case BIO_CTRL_DGRAM_QUERY_MTU:
    ret = 1400;
    break;
  case BIO_CTRL_DGRAM_SET_MTU:
    ret = num;
    break;
  case BIO_CTRL_GET_KTLS_SEND:  // TODO, investiage ktls support
    ret = 0;
    break;
  case BIO_CTRL_GET_KTLS_RECV:
    ret = 0;
    break;
  case BIO_CTRL_WPENDING:
    // anon_log("fp_ctl BIO_CTRL_WPENDING");
    break;
  case BIO_CTRL_DGRAM_SET_PEER:
    memcpy(&ioq->addr_, ptr, ioq->addr_len_);
    break;
  case BIO_CTRL_DGRAM_GET_PEER:
    memcpy(ptr, &ioq->addr_, ioq->addr_len_);
    ret = ioq->addr_len_;
    break;
  case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
    if (ioq->addr_.ss_family == AF_INET6 && memcmp(&ioq->addr_, &ipv4_in_6_header[0], 12)) {
      ret = 1280 - 48;
    } else {
      ret = 576 - 28;
    }
    break;
  default:
    anon_log("udp_sock_ctrl unknown: " << cmd);
    ret = 0;
    break;
  }
  return ret;
}

int udp_sock_puts(BIO *b, const char *str)
{
  return udp_sock_write(b, str, strlen(str));
}

int udp_sock_gets(BIO *b, char *buf, int size)
{
  int ret = 0;
  char *ptr = buf;
  char *end = buf + size - 1;

  while ((ptr < end) && (udp_sock_read(b, ptr, 1) > 0) && (ptr[0] != '\n'))
    ptr++;

  ptr[0] = 0;
  return strlen(buf);
}

///////////////////////////////////////////////////////

std::mutex mtx;
BIO_METHOD* ioq_biom = 0;
BIO_METHOD* udp_sock_biom = 0;

BIO_METHOD* get_simple_biom()
{
  std::unique_lock<std::mutex> l(mtx);
  if (ioq_biom)
    return ioq_biom;

  auto index = BIO_get_new_index();
  ioq_biom = BIO_meth_new(index, "io_queue");
  BIO_meth_set_write(ioq_biom, simple_queue_write);
  BIO_meth_set_read(ioq_biom, simple_queue_read);
  BIO_meth_set_puts(ioq_biom, simple_queue_puts);
  BIO_meth_set_gets(ioq_biom, simple_queue_gets);
  BIO_meth_set_ctrl(ioq_biom, simple_queue_ctrl);
  BIO_meth_set_create(ioq_biom, simple_queue_new);
  BIO_meth_set_destroy(ioq_biom, simple_queue_free);
  return ioq_biom;
}

BIO_METHOD* get_udp_sock_biom()
{
  std::unique_lock<std::mutex> l(mtx);
  if (udp_sock_biom)
    return udp_sock_biom;

  auto index = BIO_get_new_index();
  udp_sock_biom = BIO_meth_new(index, "upd_sock_queue");
  BIO_meth_set_write(udp_sock_biom, udp_sock_write);
  BIO_meth_set_read(udp_sock_biom, udp_sock_read);
  BIO_meth_set_puts(udp_sock_biom, udp_sock_puts);
  BIO_meth_set_gets(udp_sock_biom, udp_sock_gets);
  BIO_meth_set_ctrl(udp_sock_biom, udp_sock_ctrl);
  BIO_meth_set_create(udp_sock_biom, simple_queue_new);
  BIO_meth_set_destroy(udp_sock_biom, udp_sock_free);
  return udp_sock_biom;
}


} // namespace

BIO *BIO_new_simple_queue(const sockaddr_storage& addr)
{
  BIO *b = BIO_new(get_simple_biom());
  if (b == 0)
    return 0;
  BIO_set_data(b, new simple_queue_io(addr));
  BIO_set_retry_read(b);
  return b;
}

BIO *BIO_new_udp_sock(int fd, const sockaddr_storage& addr)
{
  BIO *b = BIO_new(get_udp_sock_biom());
  if (b == 0)
    return 0;
  BIO_set_data(b, new upd_sock_io(fd, addr));
  return b;
}
