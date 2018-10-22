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

#include <string>
#include <openssl/sha.h>
#include "big_id.h"

bool init_big_id_crypto(); // call this prior to any of the crypto functions, returns true on success, false on failure.
void term_big_id_crypto(); // call this at the end of a process's lifetime to remove any resources allocated by init_big_id_crypto().

// Function to return a big_id whose value is random (with crypto strengh randomness).
big_id rand_id();

// Function to return a big_id whose value is the SHA256 checksum value of the given 'buf'.
big_id sha256_id(const char *buf, size_t len);

inline big_id sha256_id(const std::string &str)
{
  return sha256_id(str.c_str(), str.size());
}

class sha256_builder
{
  SHA256_CTX sha256;

public:
  sha256_builder()
  {
    SHA256_Init(&sha256);
  }

  sha256_builder &operator<<(const std::string &str)
  {
    SHA256_Update(&sha256, str.c_str(), str.size() + 1);
    return *this;
  }

  sha256_builder &operator<<(const big_id &id)
  {
    SHA256_Update(&sha256, &id.m_buf[0], sizeof(id.m_buf));
    return *this;
  }

  big_id id()
  {
    uint8_t hash[big_id::id_size];
    SHA256_Final(hash, &sha256);
    return big_id(hash);
  }
};
