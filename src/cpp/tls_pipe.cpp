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

#include "tls_pipe.h"

///////////////////////////////////////////////////////////////

// an implementation of an openssl "BIO" based on anon's fiberpipe
// unlike some other non-blocking openssl implementations, this solution
// does fiber-friendly reads/writes within the read/write callback,
// making the read/write appear blocking from the perspective of openssl.

#define BIO_TYPE_FIBER_PIPE     (53|0x0400|0x0100)      /* '53' is our custom type, 4, and 1 are BIO_TYPE_SOURCE_SINK and BIO_TYPE_DESCRIPTOR */

static int fp_write(BIO *h, const char *buf, int num);
static int fp_read(BIO *h, char *buf, int size);
static int fp_puts(BIO *h, const char *str);
static int fp_gets(BIO *h, char *buf, int size);
static long fp_ctrl(BIO *h, int cmd, long arg1, void *arg2);
static int fp_new(BIO *h);
static int fp_free(BIO *data);

static BIO_METHOD methods_fdp =
{
  BIO_TYPE_FIBER_PIPE,"fiber_pipe",
  fp_write,
  fp_read,
  fp_puts,
  fp_gets,
  fp_ctrl,
  fp_new,
  fp_free,
  NULL,
};

static BIO *BIO_new_fp(std::unique_ptr<fiber_pipe>&& pipe)
{
  BIO *b = BIO_new(&methods_fdp);
  if (b == 0)
    return 0;
  b->ptr = new std::unique_ptr<fiber_pipe>(std::move(pipe));
  return b;
}

static int fp_new(BIO *b)
{
  b->init = 1;
  b->ptr=NULL;
  return 1;
}

static int fp_free(BIO *b)
{
  if (b == NULL)
    return 0;
  auto p = reinterpret_cast<std::unique_ptr<fiber_pipe>*>(b->ptr);
  if (p)
    delete p;
  b->ptr = 0;
  return 1;
}

static int fp_read(BIO *b, char *out, int outl)
{
  auto p = reinterpret_cast<std::unique_ptr<fiber_pipe>*>(b->ptr);
  if (p) {
    try {
      return (*p)->read(out, outl);
    }
    catch(...) {
    }
  }
  return -1;
}

static int fp_write(BIO *b, const char *in, int inl)
{
  auto p = reinterpret_cast<std::unique_ptr<fiber_pipe>*>(b->ptr);
  if (p) {
     try {
      (*p)->write(in, inl);
      return inl;
    }
    catch(...) {
    }
  }
  return -1;
}

static long fp_ctrl(BIO *b, int cmd, long num, void *ptr)
{
  long ret=1;

  switch (cmd) {
    case BIO_CTRL_RESET:
      anon_log("fp_ctrl BIO_CTRL_RESET");
      break;
    case BIO_CTRL_EOF:
      anon_log("fp_ctrl BIO_CTRL_EOF");
      break;
    case BIO_CTRL_INFO:
      anon_log("fp_ctrl BIO_CTRL_INFO");
      break;
    case BIO_CTRL_SET:
      anon_log("fp_ctrl BIO_CTRL_SET");
      break;
    case BIO_CTRL_GET:
      anon_log("fp_ctrl BIO_CTRL_GET");
      break;
    case BIO_CTRL_PUSH:
      //anon_log("fp_ctrl BIO_CTRL_PUSH");
      break;
    case BIO_CTRL_POP:
      //anon_log("fp_ctrl BIO_CTRL_POP");
      break;
    case BIO_CTRL_GET_CLOSE:
      anon_log("fp_ctrl BIO_CTRL_GET_CLOSE");
      break;
    case BIO_CTRL_SET_CLOSE:
      anon_log("fp_ctrl BIO_CTRL_SET_CLOSE");
      break;
    case BIO_CTRL_PENDING:
      anon_log("fp_ctrl BIO_CTRL_PENDING");
      ret = 0;
      break;
    case BIO_CTRL_FLUSH:
      //anon_log("fp_ctrl BIO_CTRL_FLUSH");
      break;
    case BIO_CTRL_DUP:
      anon_log("fp_ctrl BIO_CTRL_DUP");
      break;
    default:
      anon_log("fp_ctrl unknown: " << cmd);
      ret=0;
      break;
  }
  return ret;
}

static int fp_puts(BIO *b, const char *str)
{
  return fp_write(b, str, strlen(str));
}

static int fp_gets(BIO *b, char *buf, int size)
{
  int ret=0;
  char *ptr=buf;
  char *end=buf+size-1;

  while ((ptr < end) && (fp_read(b, ptr, 1) > 0) && (ptr[0] != '\n'))
    ptr++;

  ptr[0] = 0;
  return strlen(buf);
}

/////////////////////////////////////////////////////////////////////////////////

namespace {

struct auto_bio
{
  auto_bio(BIO* bio)
    :bio_(bio)
  {
    if (!bio_)
      throw_ssl_error();
  }
  
  ~auto_bio()
  {
    if (bio_)
      BIO_free(bio_);
  }
  
  operator BIO*() { return bio_; }
  
  BIO* release() { auto bio = bio_; bio_ = 0; return bio; }
  
  BIO *bio_;
};

}

//////////////////////////////////////////////////////////////////////////////////

tls_pipe::tls_pipe(std::unique_ptr<fiber_pipe>&& pipe, bool client, bool verify_peer, const char* host_name, const tls_context& context)
{
  auto_bio fp_bio(BIO_new_fp(std::move(pipe)));
  auto_bio ssl_bio(BIO_new_ssl(context,client));
  BIO_push(ssl_bio,fp_bio);
  
  if (BIO_do_handshake(ssl_bio) != 1)
    throw_ssl_error();

  BIO_get_ssl(ssl_bio, &ssl_);
  if (!ssl_)
    throw_ssl_error();
    
  if (verify_peer) {
    
    if (host_name) {
      X509* cert = SSL_get_peer_certificate(ssl_);
      if(cert && verify_host_name(cert,host_name)) {
        X509_free(cert);
      } else {
        if (cert)
          X509_free(cert);
        throw_ssl_error(X509_V_ERR_APPLICATION_VERIFICATION);
      }
    }
    
    auto res = SSL_get_verify_result(ssl_);
    if (res != X509_V_OK)
      throw_ssl_error((unsigned long)res);

  }
  
  ssl_bio_ = ssl_bio.release();
}

tls_pipe::~tls_pipe()
{
  // if we get here, and no one has done an SSL shutdown yet,
  // we _dont_ want to have the BIO_free try to send more data
  // (that can fail, and throw, etc...)
  SSL_set_quiet_shutdown(ssl_, 1);
  BIO_free(ssl_bio_);
}

size_t tls_pipe::read(void* buff, size_t len)
{
  auto ret = SSL_read(ssl_, buff, len);
  if (ret <= 0)
    throw_ssl_io_error(SSL_get_error(ssl_, ret));
  return ret;
}

void tls_pipe::shutdown()
{
  SSL_shutdown(ssl_);
}

#define ANON_SLOW_TLS_WRITES 50
  
void tls_pipe::write(const void* buff, size_t len)
{
  size_t tot_bytes = 0;
  const char* buf = (const char*)buff;
  while (tot_bytes < len) {
    #ifdef ANON_SLOW_TLS_WRITES
      fiber::msleep(ANON_SLOW_TLS_WRITES);
      auto written = SSL_write(ssl_, &buf[tot_bytes], 1);
    #else
      auto written = SSL_write(ssl_, &buf[tot_bytes], len-tot_bytes);
    #endif
    if (written < 0)
      throw_ssl_io_error(SSL_get_error(ssl_, written));
    tot_bytes += written;      
  }
}


