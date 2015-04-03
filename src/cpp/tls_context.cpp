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

#include "tls_context.h"
#include "fiber.h"
#include "passwords.h"

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

// basic mutex locking functions
// although we can almost use fiber_mutex's here, there is one case
// where we can have an std::mutex locked at the time we are called
// to perform these openssl locks.

#if (OPENSSL_VERSION_NUMBER & 0xf0000000) >= 0x10000000

struct CRYPTO_dynlock_value : public std::mutex
{};

static struct CRYPTO_dynlock_value *dyn_create_func(const char *file, int line)
{
  return new CRYPTO_dynlock_value;
}

static void dyn_lock_func(int mode, struct CRYPTO_dynlock_value *l, const char *file, int line)
{
  if (mode & CRYPTO_LOCK) {
    #if defined(ANON_RUNTIME_CHECKS)
    anon::inc_lock_count();
    #endif
    l->lock();
  } else {
    #if defined(ANON_RUNTIME_CHECKS)
    anon::dec_lock_count();
    #endif
    l->unlock();
  }
}

static void dyn_destroy_func(struct CRYPTO_dynlock_value *l, const char *file, int line)
{
  delete l;
}

static void threadid_func(CRYPTO_THREADID *id)
{
  CRYPTO_THREADID_set_pointer(id,get_current_fiber());
}

#endif

#include <vector>

static std::vector<std::mutex>  mutex_buff(CRYPTO_num_locks());

static void locking_func(int mode, int n, const char *file, int line)
{
  //anon_log("ssl " << (mode & CRYPTO_LOCK ? "locking" : "unlocking") << " \"" << file << "\":" << line << ", with n = " << n);
  if (mode & CRYPTO_LOCK) {
    #if defined(ANON_RUNTIME_CHECKS)
    anon::inc_lock_count();
    #endif
    mutex_buff[n].lock();
  } else {
    #if defined(ANON_RUNTIME_CHECKS)
    anon::dec_lock_count();
    #endif
    mutex_buff[n].unlock();
  }
}

#if (OPENSSL_VERSION_NUMBER & 0xf0000000) < 0x10000000
static unsigned long id_func(void)
{
  //anon_log("id_func returning " << get_current_fiber());
  return (unsigned long)get_current_fiber();
}
#endif

tls_context::fiber_init tls_context::fiber_init_;

tls_context::fiber_init::fiber_init()
{
  SSL_load_error_strings();  
  (void)SSL_library_init();
  RAND_load_file("/dev/urandom", 1024);
  
  #if (OPENSSL_VERSION_NUMBER & 0xf0000000) >= 0x10000000
  
  CRYPTO_set_dynlock_create_callback(dyn_create_func);
  CRYPTO_set_dynlock_lock_callback(dyn_lock_func);
  CRYPTO_set_dynlock_destroy_callback(dyn_destroy_func);
  CRYPTO_THREADID_set_callback(threadid_func);
  
  #endif
  
  CRYPTO_set_locking_callback(locking_func);
  
  #if (OPENSSL_VERSION_NUMBER & 0xf0000000) < 0x10000000
  CRYPTO_set_id_callback(id_func);
  #endif
}

tls_context::fiber_init::~fiber_init()
{
  ERR_free_strings();
}


/////////////////////////////////////////////////////////////////////////

// some error handling support

static const char* ssl_errors(unsigned long err)
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

void throw_ssl_error(unsigned long err)
{
  throw std::runtime_error(ssl_errors(err));
}

void throw_ssl_error()
{
  throw_ssl_error(ERR_get_error());
}

static const char* ssl_io_errors(unsigned long err)
{
  switch (err) {
    case SSL_ERROR_NONE:              return "SSL_ERROR_NONE";
    case SSL_ERROR_ZERO_RETURN:       return "SSL_ERROR_ZERO_RETURN";
    case SSL_ERROR_WANT_READ:         return "SSL_ERROR_WANT_READ";
    case SSL_ERROR_WANT_WRITE:        return "SSL_ERROR_WANT_WRITE";
    case SSL_ERROR_WANT_CONNECT:      return "SSL_ERROR_WANT_CONNECT";
    case SSL_ERROR_WANT_ACCEPT:       return "SSL_ERROR_WANT_ACCEPT";
    case SSL_ERROR_WANT_X509_LOOKUP:  return "SSL_ERROR_WANT_X509_LOOKUP";
    case SSL_ERROR_SYSCALL:           return "SSL_ERROR_SYSCALL";
    case SSL_ERROR_SSL:               return "SSL_ERROR_SSL";
  }
  return "unknown SSL io error";
}

void throw_ssl_io_error(unsigned long err)
{
  throw std::runtime_error(ssl_io_errors(err));
}

///////////////////////////////////////////////////////////////

// simple, strcmp that accepts a cert_name that starts with "*"
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

// verify that the given 'cert' was intended for the 'host_name' we
// are trying to connect to.  That is, we don't allow a connection to
// https://www.companyA.com to return a cert originally signed for
// https://www.companyB.com

bool verify_host_name(X509* const cert, const char* host_name)
{
  if (!cert)
    return false;
    
  int success = 0;
  GENERAL_NAMES* names = NULL;
  unsigned char* utf8 = NULL;

  // first try the list of "Subject Alternate Names"
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

        #if ANON_LOG_NET_TRAFFIC > 1
        if(len1 != len2)
          anon_log("Strlen and ASN1_STRING size do not match (embedded null?): " << len2 << " vs " << len1);
        #endif

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
      else {
        #if ANON_LOG_NET_TRAFFIC > 1
        anon_log("Unknown GENERAL_NAME type: " << entry->type);
        #endif
      }
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
    #if ANON_LOG_NET_TRAFFIC > 1
    anon_log("unable to verify given cert belongs to \"" << host_name << "\"");
    #endif
    return false;
  }
  return true;
}

#ifdef PRINT_CERT_INFO

// helper to stream on a Name field from an X509
template<typename T>
T& operator<<(T& str, X509_NAME *xn)
{
  char  name[200] = { 0 };
  X509_NAME_oneline(xn, &name[0], sizeof(name)-1);
  return str << name;
}

static int verify_callback(int preverify, X509_STORE_CTX* x509_ctx)
{
  anon_log("  ssl verify certificate");

  X509* cert = X509_STORE_CTX_get_current_cert(x509_ctx);
  if (cert) {
    anon_log("   Issuer:  " << X509_get_issuer_name(cert));
    anon_log("   Subject: " << X509_get_subject_name(cert));
  }

  if(preverify == 0)
    anon_log("   Error = " << ssl_errors(X509_STORE_CTX_get_error(x509_ctx)));

  #ifdef SHOW_ENTIRE_CHAIN
  return 1;
  #else
  return preverify;
  #endif
}
#endif

////////////////////////////////////////////////////////////////////

// some resource management helpers to deal with OpenSSL crappiness
namespace {

struct auto_bio_file
{
  auto_bio_file(const char* file_name)
    :bio_(BIO_new(BIO_s_file()))
  {
    if (!bio_)
      throw_ssl_error();
    if (BIO_read_filename(bio_, file_name) <= 0) {
      BIO_free(bio_);
      throw_ssl_error();
    }
  }
  
  ~auto_bio_file()
  {
    if (bio_)
      BIO_free(bio_);
  }
  
  operator BIO*() { return bio_; }
  
  BIO *bio_;
};

struct auto_x509
{
  auto_x509(X509* x)
    : x_(x)
  {
    if (!x_)
      throw_ssl_error();
  }
  
  ~auto_x509()
  {
    if (x_)
      X509_free(x_);
  }
  
  X509* release() { auto ret = x_; x_ = 0; return ret; }
  
  X509* x_;
};

struct auto_key
{
  auto_key(EVP_PKEY* key)
    : key_(key)
  {
    if (!key_)
      throw_ssl_error();
  }
  
  ~auto_key()
  {
    if (key_)
      EVP_PKEY_free(key_);
  }
  
  EVP_PKEY* release() { auto ret = key_; key_ = 0; return ret; }
  
  EVP_PKEY* key_;
};

struct auto_ctx
{
  auto_ctx(SSL_CTX* ctx)
    : ctx_(ctx)
  {
    if (!ctx_)
      throw_ssl_error();
  }
  
  ~auto_ctx()
  {
    if (ctx_)
      SSL_CTX_free(ctx_);
  }
  
  operator SSL_CTX*() { return ctx_; }
  
  SSL_CTX* release() { auto ctx = ctx_; ctx_ = 0; return ctx; }
  
  SSL_CTX* ctx_;
};

}

////////////////////////////////////////////////////////////////////

static int password_callback(char *buf, int bufsiz, int verify, void *data)
{
  int len = data ? strlen((const char*)data) : 0;
  if (len > bufsiz)
    len = bufsiz;
  memcpy(buf, data, len);
  return len;
}

static EVP_PKEY* read_pem_key(const char* key_file_name, const char* password)
{
  auto_bio_file key_bio(key_file_name);
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key_bio, NULL, password_callback, (void*)password);
  if (!pkey)
    throw_ssl_error();
  return pkey;
}

static X509* read_pem_cert(const char* cert_file_name, const char* password)
{
  auto_bio_file cert_bio(cert_file_name);
  X509* x = PEM_read_bio_X509_AUX(cert_bio, NULL, password_callback, (void*)password);
  if (!x)
    throw_ssl_error();
  return x;
}

tls_context::tls_context(bool client,
              const char* verify_cert,
              const char* verify_loc,
              const char* server_cert,
              const char* server_key,
              int verify_depth)
{
  auto_ctx ctx(SSL_CTX_new(client ? SSLv23_client_method() : SSLv23_server_method()));
  
#ifdef PRINT_CERT_INFO
  if (!client)
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_callback);
#endif

  SSL_CTX_set_verify_depth(ctx, verify_depth);

  if (!client) {
    SSL_CTX_set_quiet_shutdown(ctx,1);
    
    auto_key  key(read_pem_key(server_key, ANON_SRV_KEY_PASSWORD));
    auto_x509 cert(read_pem_cert(server_cert, ANON_SRV_CERT_PASSWORD));
    if ((SSL_CTX_use_certificate(ctx, cert.release()) <= 0)
        || (SSL_CTX_use_PrivateKey(ctx, key.release()) <= 0)
        /*|| !SSL_CTX_check_private_key(ctx)*/)
      throw_ssl_error();
  }
  
  const long flags = SSL_OP_ALL /*| SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION*/;
  (void)SSL_CTX_set_options(ctx, flags);
  
  if (verify_cert || verify_loc) {
    if (SSL_CTX_load_verify_locations(ctx, verify_cert, verify_loc) == 0)
      throw_ssl_error();
  }
    
  ctx_ = ctx.release();
}

tls_context::~tls_context()
{
  SSL_CTX_free(ctx_);
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////



