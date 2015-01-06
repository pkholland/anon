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

#pragma once

#include "fiber.h"
#include <openssl/ssl.h>

// singlton object that should exist for the life of the process.
// This object initializes openssl, and binds it to the fiber-based
// mechanism in anon.  This permits opensll to be called from fiber
// but also _requires_ that it only be called from fibers.
class tls_fiber_init
{
public:
  tls_fiber_init();
  ~tls_fiber_init();
};

class tls_pipe
{
public:
  // takes ownership of the given pipe.
  // establishes a tls handshake over this pipe, and if sucessful
  // (that does not thow an exception) all future read and write
  // calls will be encrypted/decrypted and then sent over this pipe;
  tls_pipe(std::unique_ptr<fiber_pipe>&& pipe, bool client/*vs. server*/, const char* host_name);
  
  ~tls_pipe();
    
  size_t read(void* buff, size_t len);
  void write(const void* buff, size_t len);
  
private:
  SSL_CTX*  ctx_;
  BIO*      ssl_bio_;
  SSL*      ssl_;
};

