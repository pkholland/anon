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

#include "tls_context.h"
#include "fiber.h"
#include <memory>

class tls_pipe : public pipe_t
{
public:
  // takes ownership of the given pipe.
  // establishes a tls handshake over this pipe, and if sucessful
  // (does not thow an exception) all future read and write
  // calls will be encrypted/decrypted and then sent over this pipe;
  tls_pipe(std::unique_ptr<fiber_pipe> &&pipe, bool client,
           bool verify_peer, bool doSNI, const char *host_name, const tls_context &context);

  virtual ~tls_pipe();

  virtual size_t read(void *buff, size_t len) const;
  virtual void write(const void *buff, size_t len) const;
  virtual void limit_io_block_time(int seconds);
  virtual int get_fd() const { return fp_->get_fd(); }
  virtual void set_hibernating(bool hibernating) { fp_->set_hibernating(hibernating); }
  virtual bool is_hibernating() const { return fp_->is_hibernating(); }

  void shutdown();

private:
  BIO *ssl_bio_;
  BIO *fp_bio_;
  SSL *ssl_;
  fiber_pipe *fp_;
};
