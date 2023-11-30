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

#pragma once

#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <string>

// a tls "context" object that holds a number of characteristics
// about one or more tls connections that will be attempted with
// this object.
class tls_context
{
public:
  tls_context(const tls_context& other)
    : ctx_(other.ctx_),
      sha256_digest_(other.sha256_digest_)
  {
    SSL_CTX_up_ref(ctx_);
  }

  // tls over TCP
  tls_context(bool client,             // vs. server
              const char *verify_cert, // file name of a single trusted cert (or NULL)
              const char *verify_loc,  // path to c_rehash dir of trusted certs (or NULL)
              const char *server_cert, // if !client then path to server's cert (in PEM format)
              const char *server_key,  // if !client then path to server's key (in PEM format)
              int verify_depth);       // maximum length cert chain that is allowed

  // DTLS
  tls_context(bool client,              // vs. server
              const char* cert,         // if !server and !null, path to server's cert (in PEM format)
              const char* key,          // if !server and cert, path to the server's key (in PEM format)
              int verify_depth);        // maximum length cert chain that is allowed

  ~tls_context();

  operator SSL_CTX *() const { return ctx_; }

  // currently, this only returns non-empty for DTLS contexts
  const std::string& sha256_digest() const { return sha256_digest_; }

private:
  SSL_CTX *ctx_;
  std::string sha256_digest_;

  // singlton object that should exist for the life of the process.
  // This object initializes openssl, and binds it to the fiber-based
  // mechanism in anon.  This permits opensll to be called from fiber
  // but also _requires_ that it only be called from fibers.
  class fiber_init
  {
  public:
    fiber_init();
    ~fiber_init();
  };
  static fiber_init fiber_init_;
};

void throw_ssl_error();
void throw_ssl_error(unsigned long err);
void throw_ssl_io_error(unsigned long err);
bool verify_host_name(X509 *cert, const char *host_name);
