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
  CRYPTO_set_id_callback(id_func);
}

tls_fiber_init::~tls_fiber_init()
{
  ERR_free_strings();
}

////////////////////////////////////////////////////////////////////

// an implementation of an openssl "BIO" based on anon's fiberpipe

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

// openssl is weird in the way it returns errors.  Depending on
// whether SHOW_ENTIRE_CHAIN is defined below (causing it to
// iterate throught the entire cert chain, ignoring errors) or not
// it changes the sort of error code we get back from ERR_get_error
// (further below).  So here we attempt to handle both cases of either
// direct error codes or the funky error codes that ERR_reason_error_string
// accepts.

const char* ssl_errors(unsigned long err)
{
  switch (err) {
    case X509_V_OK:                                     return "X509_V_OK";
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:          return "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT";
    case X509_V_ERR_UNABLE_TO_GET_CRL:                  return "X509_V_ERR_UNABLE_TO_GET_CRL";
    case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:   return "X509_V_OK";
    case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:    return "X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE";
    case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY: return "X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY";
    case X509_V_ERR_CERT_SIGNATURE_FAILURE:             return "X509_V_ERR_CERT_SIGNATURE_FAILURE";
    case X509_V_ERR_CRL_SIGNATURE_FAILURE:              return "X509_V_ERR_CRL_SIGNATURE_FAILURE";
    case X509_V_ERR_CERT_NOT_YET_VALID:                 return "X509_V_ERR_CERT_NOT_YET_VALID";
    case X509_V_ERR_CERT_HAS_EXPIRED:                   return "X509_V_ERR_CERT_HAS_EXPIRED";
    case X509_V_ERR_CRL_NOT_YET_VALID:                  return "X509_V_ERR_CRL_NOT_YET_VALID";
    case X509_V_ERR_CRL_HAS_EXPIRED:                    return "X509_V_ERR_CRL_HAS_EXPIRED";
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:     return "X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD";
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:      return "X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD";
    case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:     return "X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD";
    case X509_V_ERR_OUT_OF_MEM:                         return "X509_V_ERR_OUT_OF_MEM";
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:        return "X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT";
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:          return "X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN";
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:  return "X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY";
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:    return "X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE";
    case X509_V_ERR_CERT_CHAIN_TOO_LONG:                return "X509_V_ERR_CERT_CHAIN_TOO_LONG";
    case X509_V_ERR_CERT_REVOKED:                       return "X509_V_ERR_CERT_REVOKED";
    case X509_V_ERR_INVALID_CA:                         return "X509_V_ERR_INVALID_CA";
    case X509_V_ERR_PATH_LENGTH_EXCEEDED:               return "X509_V_ERR_PATH_LENGTH_EXCEEDED";
    case X509_V_ERR_INVALID_PURPOSE:                    return "X509_V_ERR_INVALID_PURPOSE";
    case X509_V_ERR_CERT_UNTRUSTED:                     return "X509_V_ERR_CERT_UNTRUSTED";
    case X509_V_ERR_CERT_REJECTED:                      return "X509_V_ERR_CERT_REJECTED";
    case X509_V_ERR_SUBJECT_ISSUER_MISMATCH:            return "X509_V_ERR_SUBJECT_ISSUER_MISMATCH";
    case X509_V_ERR_AKID_SKID_MISMATCH:                 return "X509_V_ERR_AKID_SKID_MISMATCH";
    case X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH:        return "X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH";
    case X509_V_ERR_KEYUSAGE_NO_CERTSIGN:               return "X509_V_ERR_KEYUSAGE_NO_CERTSIGN";
    case X509_V_ERR_APPLICATION_VERIFICATION:           return "X509_V_ERR_APPLICATION_VERIFICATION";
  }
  const char* er = ERR_reason_error_string(err);
  return er ? er : "unknown X509 error";
}

static void throw_ssl_error(unsigned long err)
{
  throw std::runtime_error(ssl_errors(err));
}

static void throw_ssl_error()
{
  throw_ssl_error(ERR_get_error());
}

static bool compare_to_host(const unsigned char* cert_name, const char* host_name)
{
  const char* utf8 = (const char*)cert_name;
  auto len1 = strlen(utf8);
  if (len1 > 1 && utf8[0] == '*') {
    auto pos = strstr(host_name, &utf8[1]);
    if (pos == &host_name[strlen(host_name)-(len1-1)]);
      return true;
  } else if (!strcmp((const char*)utf8, host_name))
    return true;
  return false;
}

// Defining PRINT_CERT_INFO will cause the code to
// log basic information about the certificates in the chain.
// If PRINT_CERT_INFO is defined, then also defining SHOW_ENTIRE_CHAIN
// will cause it to show this information for each certificate
// in the chain, regardless of whether that certificate is considered
// "valid" or not.  Without SHOW_ENTIRE_CHAIN defined it will
// stop on the first invalid certificate.  HOWEVER, as a subtle
// by-product of this, defining SHOW_ENTIRE_CHAIN also changes the
// error code seen (and then thrown) by BIO_do_handshake in the
// case where certificate chain cannot be verified -- that is, there
// is an error.
//#define PRINT_CERT_INFO
//#define SHOW_ENTIRE_CHAIN

static bool verify_host_name(X509* const cert, const char* host_name)
{
  if (!cert)
    return false;
    
  int success = 0;
  GENERAL_NAMES* names = NULL;
  unsigned char* utf8 = NULL;

  // first try the "Subject Alternate Names"
  do
  {
    names = (GENERAL_NAMES*)X509_get_ext_d2i(cert, NID_subject_alt_name, 0, 0);
    if(!names) break;

    int i = 0, count = sk_GENERAL_NAME_num(names);
    if(!count) break;

    for( i = 0; i < count && !success; ++i ) {
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
        #ifdef PRINT_CERT_INFO
        if (utf8 && len1 && len2 && (len1 == len2))
          anon_log("  comparing hostname \"" << host_name << "\" to certificate's Subject Alternate Name \"" << (const char*)utf8 << "\"");
        #endif
        if(utf8 && len1 && len2 && (len1 == len2) && compare_to_host(utf8, host_name)) {
          #ifdef PRINT_CERT_INFO
          anon_log("   names match, certificate is issued to this host");
          #endif
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
    
  // now try the common name field of the subject
  if (!success) {
    X509_NAME* sname = X509_get_subject_name(cert);
    if (sname) {
      int idx = X509_NAME_get_index_by_NID(sname, NID_commonName, -1);
      if (idx != -1) {
        X509_NAME_ENTRY* entry = X509_NAME_get_entry(sname,idx);
        if (entry) {
          ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
          if (data) {
            unsigned char* utf8 = 0;
            int length = ASN1_STRING_to_UTF8(&utf8, data);
            if (utf8 != 0) {
              #ifdef PRINT_CERT_INFO
              if (length > 0)
                anon_log("  comparing hostname \"" << host_name << "\" to certificate's Subject Common Name \"" << (const char*)utf8 << "\"");
              #endif
              if (length > 0 && compare_to_host(utf8, host_name)) {
                #ifdef PRINT_CERT_INFO
                anon_log("   names match, certificate is issued to this host");
                #endif
                success = 1;
              }
              OPENSSL_free(utf8);
            }
          }
        }
      }
    }
  }

  if(!success)
  {
    anon_log("unable to verify given cert belongs to " << host_name);
    return false;
  }
  return true;
}

#ifdef PRINT_CERT_INFO

template<typename T>
T& operator<<(T& str, X509_NAME *xn)
{
  char  name[200] = { 0 };
  X509_NAME_oneline(xn, &name[0], sizeof(name)-1);
  return str << name;
}

static int verify_callback(int preverify, X509_STORE_CTX* x509_ctx)
{
  anon_log("  ssl verify certificate chain depth: " << X509_STORE_CTX_get_error_depth(x509_ctx));

  X509* cert = X509_STORE_CTX_get_current_cert(x509_ctx);
  if (cert) {
    anon_log("   Issuer:  " << X509_get_issuer_name(cert));
    anon_log("   Subject: " << X509_get_subject_name(cert));
  }

  /* For error codes, see http://www.openssl.org/docs/apps/verify.html  */
  if(preverify == 0)
    anon_log("   Error = " << ssl_errors(X509_STORE_CTX_get_error(x509_ctx)));

  #ifdef SHOW_ENTIRE_CHAIN
  return 1;
  #else
  return preverify;
  #endif
}
#endif

tls_pipe::tls_pipe(std::unique_ptr<fiber_pipe>&& pipe, bool client/*vs. server*/, const char* host_name)
{
  ctx_ = SSL_CTX_new(client ? SSLv23_client_method() : SSLv23_server_method());
  if (ctx_ == 0)
    throw_ssl_error();
#ifdef PRINT_CERT_INFO
  SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_callback);
#endif
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
  ssl_bio_ = BIO_new_ssl(ctx_,client);
  if (!ssl_bio_) {
    auto ec = ERR_get_error();
    BIO_free(fpb);
    SSL_CTX_free(ctx_);
    throw_ssl_error(ec);
  }
  BIO_push(ssl_bio_,fpb);
  
  if (BIO_do_handshake(ssl_bio_) != 1) {
    anon_log("BIO_do_handshake returned error");
    auto ec = ERR_get_error();
    ERR_print_errors_cb([](const char *str, size_t len, void *u)->int{anon_log(str); return 1;}, 0);
    BIO_free(ssl_bio_);
    SSL_CTX_free(ctx_);
    throw_ssl_error(ec);
  }
  
  BIO_get_ssl(ssl_bio_, &ssl_);
  
  if (!ssl_) {
    auto ec = ERR_get_error();
    BIO_free(ssl_bio_);
    SSL_CTX_free(ctx_);
    throw_ssl_error(ec);
  }
  
  if (client) {
    
    /* Step 1: verify a server certifcate was presented during negotiation
              Anonymous Diffie-Hellman (ADH) is not allowed
    */
    X509* cert = SSL_get_peer_certificate(ssl_);
    if(cert && verify_host_name(cert,host_name)) {
      X509_free(cert); /* Free immediately */
    } else {
      if (cert)
        X509_free(cert);
      BIO_free(ssl_bio_);
      SSL_CTX_free(ctx_);
      throw_ssl_error(X509_V_ERR_APPLICATION_VERIFICATION);
    }
    
    auto res = SSL_get_verify_result(ssl_);
    if (res != X509_V_OK) {
      BIO_free(ssl_bio_);
      SSL_CTX_free(ctx_);
      throw_ssl_error((unsigned long)res);
    }

  }

  
}

tls_pipe::~tls_pipe()
{
  BIO_free(ssl_bio_);
  SSL_CTX_free(ctx_);
}

void tls_pipe::write(const void* buff, size_t len)
{
  size_t tot_bytes = 0;
  const char* buf = (const char*)buff;
  while (tot_bytes < len) {
    auto written = SSL_write(ssl_, &buf[tot_bytes], len-tot_bytes);
    if (written < 0)
      throw_ssl_error(SSL_get_error(ssl_, id_func()));
    tot_bytes += written;      
  }
}



