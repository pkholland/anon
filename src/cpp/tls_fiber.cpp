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

#include "tls_fiber.h"

#define OPENSSL_THREAD_DEFINES
#include <openssl/opensslconf.h>
#if defined(OPENSSL_THREADS)
   // ok, this is what we want
#else
   #error SSL thread support disabled, cannot build
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>


#include <vector>

static std::vector<fiber_mutex>  mutex_buff(CRYPTO_num_locks());

static void locking_func(int mode,int n, const char *file,int line)
{
  //anon_log("ssl " << (mode & CRYPTO_LOCK ? "locking" : "unlocking") << " \"" << file << "\":" << line << ", with n = " << n);
  if (mode & CRYPTO_LOCK)
    mutex_buff[n].lock();
  else
    mutex_buff[n].unlock();
}

static unsigned long id_func(void)
{
  //anon_log("id_func returning " << get_current_fiber());
  return (unsigned long)get_current_fiber();
} 

tls_fiber_init::tls_fiber_init()
{
  (void)SSL_library_init();
  SSL_load_error_strings();  
  RAND_load_file("/dev/urandom", 1024);
  
  CRYPTO_set_locking_callback(locking_func);
  //CRYPTO_set_id_callback(id_func);
}

tls_fiber_init::~tls_fiber_init()
{
  ERR_free_strings();
}

////////////////////////////////////////////////////////////////////

#define BIO_TYPE_FIBER_PIPE	(53|0x0400|0x0100)	/* '53' is our custom type, 4, and 1 are BIO_TYPE_SOURCE_SINK and BIO_TYPE_DESCRIPTOR */

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
  if (p)
    return (*p)->read(out, outl);
  return -1;
}

static int fp_write(BIO *b, const char *in, int inl)
{
  auto p = reinterpret_cast<std::unique_ptr<fiber_pipe>*>(b->ptr);
  if (p) {
    (*p)->write(in, inl);
    return inl;
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

////////////////////////////////////////////////////////////////////

static void throw_ssl_error(unsigned long err)
{
  const char* er = ERR_reason_error_string(err);
  if (er)
    throw std::runtime_error(er);
  throw std::runtime_error("unknown openssl error");
}

static void throw_ssl_error()
{
  throw_ssl_error(ERR_get_error());
}

//#define PRINT_CERT_INFO

#ifdef PRINT_CERT_INFO
static void print_cn_name(const char* label, X509_NAME* const name)
{
  int idx = -1, success = 0;
  unsigned char *utf8 = NULL;

  do
  {
    if(!name) break; /* failed */

    idx = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if(!(idx > -1))  break; /* failed */

    X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, idx);
    if(!entry) break; /* failed */

    ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
    if(!data) break; /* failed */

    int length = ASN1_STRING_to_UTF8(&utf8, data);
    if(!utf8 || !(length > 0))  break; /* failed */

    anon_log(label << ": " << (char*)utf8);

    success = 1;

  } while (0);

  if(utf8)
    OPENSSL_free(utf8);

  if(!success)
    anon_log(label << ": <not available>");
}

void print_san_name(const char* label, X509* const cert)
{
  int success = 0;
  GENERAL_NAMES* names = NULL;
  unsigned char* utf8 = NULL;

  do
  {
    if(!cert) break; /* failed */

    names = (GENERAL_NAMES*)X509_get_ext_d2i(cert, NID_subject_alt_name, 0, 0);
    if(!names) break;

    int i = 0, count = sk_GENERAL_NAME_num(names);
    if(!count) break; /* failed */

    for( i = 0; i < count; ++i ) {
      GENERAL_NAME* entry = sk_GENERAL_NAME_value(names, i);
      if(!entry) continue;

      if(GEN_DNS == entry->type) {
        int len1 = 0, len2 = -1;

        len1 = ASN1_STRING_to_UTF8(&utf8, entry->d.dNSName);
        if(utf8)
          len2 = (int)strlen((const char*)utf8);

        if(len1 != len2)
          anon_log("Strlen and ASN1_STRING size do not match (embedded null?): " << len2 << " vs " << len1);

        /* If there's a problem with string lengths, then     */
        /* we skip the candidate and move on to the next.     */
        /* Another policy would be to fails since it probably */
        /* indicates the client is under attack.              */
        if(utf8 && len1 && len2 && (len1 == len2)) {
          anon_log(label << ": " << utf8);
          success = 1;
        }

        if(utf8)
          OPENSSL_free(utf8), utf8 = NULL;
      }
      else
        anon_log("Unknown GENERAL_NAME type: " << entry->type);
    }

  } while (0);

  if(names)
    GENERAL_NAMES_free(names);

  if(utf8)
    OPENSSL_free(utf8);

  if(!success)
    anon_log(label << ": not available");
}
#endif

static int verify_callback(int preverify, X509_STORE_CTX* x509_ctx)
{
  /* For error codes, see http://www.openssl.org/docs/apps/verify.html  */

  int depth = X509_STORE_CTX_get_error_depth(x509_ctx);
  int err = X509_STORE_CTX_get_error(x509_ctx);

  X509* cert = X509_STORE_CTX_get_current_cert(x509_ctx);
  X509_NAME* iname = cert ? X509_get_issuer_name(cert) : NULL;
  X509_NAME* sname = cert ? X509_get_subject_name(cert) : NULL;

#ifdef PRINT_CERT_INFO
  anon_log("ssl verify_callback depth: " << depth << ", preverify: " << preverify);

  /* Issuer is the authority we trust that warrants nothing useful */
  print_cn_name("Issuer (cn)", iname);

  /* Subject is who the certificate is issued to by the authority  */
  print_cn_name("Subject (cn)", sname);

  if(depth == 0) {
    /* If depth is 0, its the server's certificate. Print the SANs */
    print_san_name("Subject (san)", cert);
  }

  if(preverify == 0)
  {
    if(err == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY)
      anon_log("Error = X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY");
    else if(err == X509_V_ERR_CERT_UNTRUSTED)
      anon_log("Error = X509_V_ERR_CERT_UNTRUSTED");
    else if(err == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN)
      anon_log("Error = X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN");
    else if(err == X509_V_ERR_CERT_NOT_YET_VALID)
      anon_log("Error = X509_V_ERR_CERT_NOT_YET_VALID");
    else if(err == X509_V_ERR_CERT_HAS_EXPIRED)
      anon_log("Error = X509_V_ERR_CERT_HAS_EXPIRED");
    else if(err == X509_V_OK)
      anon_log("Error = X509_V_OK\n");
    else
      anon_log("Error = " << err);
  }
#endif

  #if defined(IGNORE_ERRORS)
  return 1;
  #else
  return preverify;
  #endif
}

tls_pipe::tls_pipe(std::unique_ptr<fiber_pipe>&& pipe, bool client/*vs. server*/, const char* host_name)
{
  ctx_ = SSL_CTX_new(client ? SSLv23_client_method() : SSLv23_server_method());
  if (ctx_ == 0)
    throw_ssl_error();
  SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_callback);
  SSL_CTX_set_verify_depth(ctx_, 5);

  const long flags = SSL_OP_ALL /*| SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3*/ | SSL_OP_NO_COMPRESSION;
  (void)SSL_CTX_set_options(ctx_, flags);
  
  if (SSL_CTX_load_verify_locations(ctx_, 0, "/etc/ssl/certs") == 0) {
    auto ec = ERR_get_error();
    SSL_CTX_free(ctx_);
    throw_ssl_error(ec);
  }
    
  BIO* fpb = BIO_new_fp(std::move(pipe));
  if (!fpb) {
    auto ec = ERR_get_error();
    SSL_CTX_free(ctx_);
    throw_ssl_error(ec);
  }
  ssl_ = BIO_new_ssl(ctx_,client);
  if (!ssl_) {
    auto ec = ERR_get_error();
    BIO_free(fpb);
    SSL_CTX_free(ctx_);
    throw_ssl_error(ec);
  }
  BIO_push(ssl_,fpb);
  
  if (BIO_do_handshake(ssl_) != 1) {
    anon_log("BIO_do_handshake returned error");
    auto ec = ERR_get_error();
    anon_log("ERR_get_error returned: " << ec);
    BIO_free(ssl_);
    SSL_CTX_free(ctx_);
    throw_ssl_error(ec);
  }
  
  SSL *ssl = 0;
  BIO_get_ssl(ssl_, &ssl);
  
  if (client) {
    
    /* Step 1: verify a server certifcate was presented during negotiation
              Anonymous Diffie-Hellman (ADH) is not allowed
    */
    X509* cert = SSL_get_peer_certificate(ssl);
    if(cert)
      X509_free(cert); /* Free immediately */
    else {
      BIO_free(ssl_);
      SSL_CTX_free(ctx_);
      throw_ssl_error(X509_V_ERR_APPLICATION_VERIFICATION);
    }
    
    auto res = SSL_get_verify_result(ssl);
    if (res != X509_V_OK) {
      anon_log("SSL_get_verify_result returned error: " << res);
      BIO_free(ssl_);
      SSL_CTX_free(ctx_);
      throw_ssl_error((unsigned long)res);
    }
        
    // check host_name
    
    anon_log("handshake completed, certificates accepted, ready to communicate");    
    
  }

  
}



