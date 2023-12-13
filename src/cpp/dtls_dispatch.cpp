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

#include "dtls_dispatch.h"
#include "dtls_bio.h"
#include "fiber.h"
#include "log.h"
#include "sctp_dispatch.h"
#include "tcp_utils.h"
#include <openssl/ssl.h>
#include <vector>

struct sctp_association : public std::enable_shared_from_this<sctp_association> {
  SSL* ssl = nullptr;
  bool listened = false;
  bool accepted = false;
  std::shared_ptr<sctp_dispatch> sctp;
  timespec last_used_time;

  sctp_association(const sockaddr_storage& client_addr, int fd, SSL_CTX* dtls_context)
    : last_used_time(cur_time())
  {
    auto read_bio = BIO_new_simple_queue(client_addr);
    auto write_bio = BIO_new_udp_sock(fd, client_addr);
    ssl = SSL_new(dtls_context);
    SSL_set_bio(ssl, read_bio, write_bio);
    SSL_set_options(ssl, SSL_OP_COOKIE_EXCHANGE);
  }

  void set_sctp(uint16_t local_port, uint16_t remote_port)
  {
    sctp = std::make_shared<sctp_dispatch>(local_port, remote_port);
    sctp->connect(
      [wths = std::weak_ptr<sctp_association>(shared_from_this())]
      (const uint8_t* msg, size_t len)
      {
        if (auto ths = wths.lock()) {
          SSL_write(ths->ssl, msg, len);
        }
      });
  }

  sctp_association() = default;

  ~sctp_association()
  {
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
    }
  }

  void recv_msg(const uint8_t *msg, ssize_t len)
  {
    last_used_time = cur_time();
    auto b = SSL_get_rbio(ssl);
    if (b) {
      BIO_write(b, msg, len);
    }
    else {
      return;
    }

    if (!listened) {
      sockaddr_storage client_addr;
      if (DTLSv1_listen(ssl, (BIO_ADDR*)&client_addr) <= 0) {
        return;
      }
      listened = true;
    }

    if (!accepted) {
      if (SSL_accept(ssl) <= 0) {
        return;
      }
      accepted = true;
    }

    std::vector<uint8_t> decrypted(len+100);
    while (true) {
      auto ret = SSL_read(ssl, &decrypted[0], decrypted.size());
      if (ret <= 0) {
        return;
      }
      sctp->recv_msg(&decrypted[0], ret);
    }
  }
};


dtls_dispatch::dtls_dispatch(const std::shared_ptr<tls_context>& dtls_context, int udp_fd)
  : dtls_context(dtls_context),
    udp_fd(udp_fd)
{
}

void dtls_dispatch::sweep_inactive()
{
  sweep_task = fiber::schedule_task([wths = std::weak_ptr<dtls_dispatch>(shared_from_this())](){
    if (auto ths = wths.lock()) {
      auto now = cur_time();
      fiber_lock l(ths->dtls_mtx);
      for (auto it = ths->sctp_associations.begin(); it != ths->sctp_associations.end();) {
        if (it->second->last_used_time + 30 < now) {
          it = ths->sctp_associations.erase(it);
        }
        else {
          it++;
        }
      }
      if (ths->sctp_associations.size() > 0) {
        ths->sweep_inactive();
      }
    }
  }, cur_time() + 30);
}

void dtls_dispatch::register_association(const struct sockaddr_storage *sockaddr, uint16_t local_sctp_port, uint16_t remote_sctp_port)
{
  fiber_lock l(dtls_mtx);
  auto it = sctp_associations.find(*sockaddr);
  if (it == sctp_associations.end()) {
    auto conn = std::make_shared<sctp_association>(*sockaddr, udp_fd, *dtls_context);
    conn->set_sctp(local_sctp_port, remote_sctp_port);
    sctp_associations.emplace(std::make_pair(*sockaddr, conn));
    if (sctp_associations.size() == 1) {
      sweep_inactive();
    }
  }
}

void dtls_dispatch::recv_msg(const uint8_t *msg,
              ssize_t len,
              const struct sockaddr_storage *sockaddr)
{
  fiber_lock l(dtls_mtx);
  auto it = sctp_associations.find(*sockaddr);
  if (it != sctp_associations.end()) {
    auto conn = it->second;
    l.unlock();
    conn->recv_msg(msg, len);
  }
  else {
    anon_log("possible DTLS message from unknown source addr: " << *sockaddr);
  }
}
