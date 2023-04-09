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

#include "big_id_crypto.h"
#include "log.h"
#include <fcntl.h>
#include <unistd.h>
#include <iostream>

static int rand_file = -1;

bool init_big_id_crypto()
{
  if (rand_file != -1)
    return false;
  rand_file = open("/dev/urandom", O_RDONLY);
  if (rand_file != -1)
    anon_log("using fd " << rand_file << " for reading from /dev/urandom");
  return rand_file != -1;
}

void term_big_id_crypto()
{
  if (rand_file != -1)
  {
    close(rand_file);
    rand_file = -1;
  }
}

big_id big_rand_id()
{
  uint8_t rid[big_id::id_size];
  if (read(rand_file, &rid[0], sizeof(rid)) != sizeof(rid))
    anon_log_error("reading from rand_file (fd = " << rand_file << ") failed with errno: " << errno_string());
  return big_id(rid);
}

// the sha256sum of the given 'buf' as a big_id
big_id sha256_id(const char *buf, size_t len)
{
  sha256_builder builder;
  builder.update(buf, len);
  return builder.id();
}

small_id small_rand_id()
{
  uint8_t rid[small_id::id_size];
  if (read(rand_file, &rid[0], sizeof(rid)) != sizeof(rid))
    anon_log_error("reading from rand_file (fd = " << rand_file << ") failed with errno: " << errno_string());
  return small_id(rid);
}

// the sha1sum of the given 'buf' as a big_id
small_id sha1_id(const char *buf, size_t len)
{
  sha1_builder builder;
  builder.update(buf, len);
  return builder.id();
}
