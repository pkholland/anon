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

struct dtls_connection {
  SSL* ssl = nullptr;
  bool listened = false;
  bool accepted = false;
  std::shared_ptr<sctp_dispatch> sctp;
  timespec last_used_time;

  dtls_connection(const sockaddr_storage& client_addr, int fd, SSL_CTX* dtls_context)
    : sctp(std::make_shared<sctp_dispatch>()),
      last_used_time(cur_time())
  {
    auto read_bio = BIO_new_simple_queue(client_addr);
    auto write_bio = BIO_new_udp_sock(fd, client_addr);
    ssl = SSL_new(dtls_context);
    SSL_set_bio(ssl, read_bio, write_bio);
    SSL_set_options(ssl, SSL_OP_COOKIE_EXCHANGE);
  }

  dtls_connection() = default;

  ~dtls_connection()
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
      anon_log("woot! we be accepted!!!");
    }

    std::vector<uint8_t> decrypted(len + 20);
    auto ret = SSL_read(ssl, &decrypted[0], decrypted.size());
    if (ret < 0) {
      anon_log("decryption failed");
      return;
    }

    sctp->recv_msg(&decrypted[0], ret);
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
      for (auto it = ths->dtls_connections.begin(); it != ths->dtls_connections.end();) {
        if (it->second->last_used_time + 30 < now) {
          it = ths->dtls_connections.erase(it);
        }
        else {
          it++;
        }
      }
      if (ths->dtls_connections.size() > 0) {
        ths->sweep_inactive();
      }
    }
  }, cur_time() + 30);
}

void dtls_dispatch::register_address(const struct sockaddr_storage *sockaddr)
{
  fiber_lock l(dtls_mtx);
  auto it = dtls_connections.find(*sockaddr);
  if (it == dtls_connections.end()) {
    auto conn = std::make_shared<dtls_connection>(*sockaddr, udp_fd, *dtls_context);
    dtls_connections.emplace(std::make_pair(*sockaddr, conn));
    if (dtls_connections.size() == 1) {
      sweep_inactive();
    }
  }
}

void dtls_dispatch::recv_msg(const uint8_t *msg,
              ssize_t len,
              const struct sockaddr_storage *sockaddr)
{
  fiber_lock l(dtls_mtx);
  auto it = dtls_connections.find(*sockaddr);
  if (it != dtls_connections.end()) {
    auto conn = it->second;
    l.unlock();
    conn->recv_msg(msg, len);
  }
  else {
    anon_log("possible DTLS message from unknown source addr");
  }
}
