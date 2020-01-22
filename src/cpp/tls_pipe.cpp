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

#include "tls_pipe.h"
#include <openssl/opensslv.h>

///////////////////////////////////////////////////////////////

// an implementation of an openssl "BIO" based on anon's fiberpipe
// unlike some other non-blocking openssl implementations, this solution
// does fiber-friendly reads/writes within the read/write callback,
// making the read/write appear blocking from the perspective of openssl.

#define BIO_TYPE_FIBER_PIPE (53 | 0x0400 | 0x0100) /* '53' is our custom type, 4, and 1 are BIO_TYPE_SOURCE_SINK and BIO_TYPE_DESCRIPTOR */

static int fp_write(BIO *h, const char *buf, int num);
static int fp_read(BIO *h, char *buf, int size);
static int fp_puts(BIO *h, const char *str);
static int fp_gets(BIO *h, char *buf, int size);
static long fp_ctrl(BIO *h, int cmd, long arg1, void *arg2);
static int fp_new(BIO *h);
static int fp_free(BIO *data);

// original openssl, before 1.1 needed different code
// to initialize a custom BIO
#if OPENSSL_VERSION_NUMBER < 0x10100000

static BIO_METHOD methods_fdp =
    {
        BIO_TYPE_FIBER_PIPE,
        "fiber_pipe",
        fp_write,
        fp_read,
        fp_puts,
        fp_gets,
        fp_ctrl,
        fp_new,
        fp_free,
        NULL,
};

#endif

namespace
{

#if OPENSSL_VERSION_NUMBER < 0x10100000

void BIO_set_data(BIO *b, void* p) {
  b->ptr = p;
}

void* BIO_get_data(BIO* b) {
  return b->ptr;
}

BIO_METHOD* create_biom() {
  return &methods_fdp;
}

void BIO_set_init(BIO *b, int init) {
  b->init = init;
}

#else

std::mutex mtx;
BIO_METHOD* biom = 0;

BIO_METHOD* create_biom()
{
  std::unique_lock<std::mutex> l(mtx);
  if (biom)
    return biom;

  auto index = BIO_get_new_index();
  biom = BIO_meth_new(index, "fiber_pipe");
  BIO_meth_set_write(biom, fp_write);
  BIO_meth_set_read(biom, fp_read);
  BIO_meth_set_puts(biom, fp_puts);
  BIO_meth_set_gets(biom, fp_gets);
  BIO_meth_set_ctrl(biom, fp_ctrl);
  BIO_meth_set_create(biom, fp_new);
  BIO_meth_set_destroy(biom, fp_free);
  return biom;
}

#endif

class fp_pipe
{
public:
  fp_pipe(std::unique_ptr<fiber_pipe> &&pipe)
      : pipe_(std::move(pipe)),
        hit_fiber_io_error_(false),
        hit_fiber_io_timeout_error_(false)
  {
  }

  std::unique_ptr<fiber_pipe> pipe_;
  bool hit_fiber_io_error_;
  bool hit_fiber_io_timeout_error_;
};

} // namespace

static BIO *BIO_new_fp(std::unique_ptr<fiber_pipe> &&pipe)
{
  BIO *b = BIO_new(create_biom());
  if (b == 0)
    return 0;
  BIO_set_data(b, new fp_pipe(std::move(pipe)));
  return b;
}

static int fp_new(BIO *b)
{
  BIO_set_init(b, 1);
  BIO_set_data(b, 0);
  return 1;
}

static int fp_free(BIO *b)
{
  if (b == NULL)
    return 0;
  auto p = reinterpret_cast<fp_pipe *>(BIO_get_data(b));
  if (p)
    delete p;
  BIO_set_data(b, 0);
  return 1;
}

static int fp_read(BIO *b, char *out, int outl)
{
  auto p = reinterpret_cast<fp_pipe *>(BIO_get_data(b));
  if (p)
  {
    try
    {
      int ret = p->pipe_->read(out, outl);
      return ret;
    }
    catch (const fiber_io_error &)
    {
      p->hit_fiber_io_error_ = true;
    }
    catch(const fiber_io_timeout_error &)
    {
      p->hit_fiber_io_timeout_error_ = true;
    }
    catch (...)
    {
    }
  }
  return -1;
}

static int fp_write(BIO *b, const char *in, int inl)
{
  auto p = reinterpret_cast<fp_pipe *>(BIO_get_data(b));
  if (p)
  {
    try
    {
      p->pipe_->write(in, inl);
      return inl;
    }
    catch (const fiber_io_error &)
    {
      p->hit_fiber_io_error_ = true;
    }
    catch (const fiber_io_timeout_error &)
    {
      p->hit_fiber_io_timeout_error_ = true;
    }
    catch (...)
    {
    }
  }
  return -1;
}

static long fp_ctrl(BIO *b, int cmd, long num, void *ptr)
{
  long ret = 1;

  switch (cmd)
  {
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
  case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
    // anon_log("fp_ctrl BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT");
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
  case BIO_CTRL_WPENDING:
    // anon_log("fp_ctl BIO_CTRL_WPENDING");
    break;
  default:
    anon_log("fp_ctrl unknown: " << cmd);
    ret = 0;
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
  int ret = 0;
  char *ptr = buf;
  char *end = buf + size - 1;

  while ((ptr < end) && (fp_read(b, ptr, 1) > 0) && (ptr[0] != '\n'))
    ptr++;

  ptr[0] = 0;
  return strlen(buf);
}

/////////////////////////////////////////////////////////////////////////////////

namespace
{

struct auto_bio
{
  auto_bio(BIO *bio)
      : bio_(bio)
  {
    if (!bio_)
      throw_ssl_error();
  }

  ~auto_bio()
  {
    if (bio_)
      BIO_free(bio_);
  }

  operator BIO *() { return bio_; }

  BIO *release()
  {
    auto bio = bio_;
    bio_ = 0;
    return bio;
  }

  BIO *bio_;
};

void throw_ssl_error_(BIO *fpb)
{
  auto p = reinterpret_cast<fp_pipe *>(BIO_get_data(fpb));
  if (p->hit_fiber_io_error_)
    anon_throw(fiber_io_error, "fiber io error during tls 1");
  else if (p->hit_fiber_io_timeout_error_)
    anon_throw(fiber_io_timeout_error, "fiber io timeout error during tls 1");
  else
    throw_ssl_error();
}

static void throw_ssl_error_(BIO *fpb, unsigned long err)
{
  auto p = reinterpret_cast<fp_pipe *>(BIO_get_data(fpb));
  if (p->hit_fiber_io_error_)
    anon_throw(fiber_io_error, "fiber io error during tls 2");
  else if (p->hit_fiber_io_timeout_error_)
    anon_throw(fiber_io_timeout_error, "fiber io timeout error during tls 2");
  else
    throw_ssl_error(err);
}

void throw_ssl_io_error_(BIO *fpb, unsigned long err)
{
  auto p = reinterpret_cast<fp_pipe *>(BIO_get_data(fpb));
  if (p->hit_fiber_io_error_)
    anon_throw(fiber_io_error, "fiber io error during tls 3");
  else if (p->hit_fiber_io_timeout_error_)
    anon_throw(fiber_io_timeout_error, "fiber io timeout error during tls 3");
  else
    throw_ssl_io_error(err);
}

} // namespace

//////////////////////////////////////////////////////////////////////////////////

tls_pipe::tls_pipe(std::unique_ptr<fiber_pipe> &&pipe, bool client, bool verify_peer,
                   bool doSNI, const char *host_name, const tls_context &context)
    : fp_(pipe.get())
{
  auto_bio fp_bio(BIO_new_fp(std::move(pipe)));
  auto_bio ssl_bio(BIO_new_ssl(context, client));
  BIO_push(ssl_bio, fp_bio);

  BIO_get_ssl(ssl_bio, &ssl_);
  if (!ssl_)
    throw_ssl_error_(fp_bio);

  if (doSNI && host_name)
    SSL_set_tlsext_host_name(ssl_, host_name);

  if (BIO_do_handshake(ssl_bio) != 1)
    throw_ssl_error_(fp_bio);

  if (verify_peer)
  {

    if (host_name)
    {
      X509 *cert = SSL_get_peer_certificate(ssl_);
      if (cert && verify_host_name(cert, host_name))
      {
        X509_free(cert);
      }
      else
      {
        if (cert)
          X509_free(cert);
        throw_ssl_error_(fp_bio, X509_V_ERR_APPLICATION_VERIFICATION);
      }
    }

    auto res = SSL_get_verify_result(ssl_);
    if (res != X509_V_OK)
      throw_ssl_error_(fp_bio, (unsigned long)res);
  }

  ssl_bio_ = ssl_bio.release();
  fp_bio_ = fp_bio.release();
}

tls_pipe::~tls_pipe()
{
  // if we get here, and no one has done an SSL shutdown yet,
  // we _dont_ want to have the BIO_free try to send more data
  // (that can fail, and throw, etc...)
  SSL_set_quiet_shutdown(ssl_, 1);
  BIO_free(ssl_bio_);
  BIO_free(fp_bio_);
}

size_t tls_pipe::read(void *buff, size_t len) const
{
  auto ret = SSL_read(ssl_, buff, len);
  if (ret <= 0)
    throw_ssl_io_error_(fp_bio_, SSL_get_error(ssl_, ret));
  return ret;
}

void tls_pipe::shutdown()
{
  SSL_shutdown(ssl_);
}

//#define ANON_SLOW_TLS_WRITES 50

void tls_pipe::write(const void *buff, size_t len) const
{
  size_t tot_bytes = 0;
  const char *buf = (const char *)buff;
  while (tot_bytes < len)
  {
#ifdef ANON_SLOW_TLS_WRITES
    fiber::msleep(ANON_SLOW_TLS_WRITES);
    auto written = SSL_write(ssl_, &buf[tot_bytes], 1);
#else
    auto written = SSL_write(ssl_, &buf[tot_bytes], len - tot_bytes);
#endif
    if (written < 0)
      throw_ssl_io_error_(fp_bio_, SSL_get_error(ssl_, written));
    tot_bytes += written;
  }
}

void tls_pipe::limit_io_block_time(int seconds)
{
  fp_->limit_io_block_time(seconds);
}
