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

#include "big_id.h"
#include "log.h"
#include <iostream>

// a couple of helper functions that are private to the implementation
namespace priv {

// Fill 'ascii' with the ascii/hex version of what it is 'id'.
inline void to_ascii_hex(char (&ascii)[big_id::id_size*2 + 1], const big_id& id)
{
  for (int i = 0; i < big_id::id_size; i++) {
    char nib = id.m_buf[i] >> 4;
    if (nib < 10)
      ascii[i*2] = '0' + nib;
    else
      ascii[i*2] = 'a' + nib-10;
    nib = id.m_buf[i] & 15;
    if (nib < 10)
      ascii[i*2+1] = '0' + nib;
    else
      ascii[i*2+1] = 'a' + nib-10;
  }
  ascii[sizeof(ascii)-1] = 0;
}

// Convert one ascii/hex character to its integer value value.
inline uint8_t hex_to_i(char nib)
{
  if (nib >= '0' && nib <= '9')
    return nib - '0';
  if (nib >= 'a' && nib <= 'f')
    return nib - 'a' + 10;
  if (nib >= 'A' && nib <= 'F')
    return nib - 'A' + 10;
  anon_log_error("invalid hex value! " << nib);
  return 0;
}

}

// helper class for "long display" of a big_id.
struct long_big_id : big_id
{};


// helper function for streaming out the "long display" form of a big_id.
// Use this if you want to override the default streaming big_id format to always stream all 32 bytes.
inline const long_big_id& ldisp(const big_id& id)
{
  return *static_cast<const long_big_id*>(&id);
}


template<typename T>
T& operator<<(T& str, const long_big_id& id)
{
  char ascii[big_id::id_size*2 + 1];
  priv::to_ascii_hex(ascii, id);
  return str << &ascii[0];
}

// helper class for "short display" of a big_id.
struct short_big_id : big_id
{};

// helper function for streaming out the "short display" form of a big_id.
inline const short_big_id& sdisp(const big_id& id)
{
  return *static_cast<const short_big_id*>(&id);
}

// template function to stream out a big_id, in ascii/hex form.  Will write 64 bytes to the given stream.
template<typename T>
T& operator<<(T& str, const big_id& id)
{
  return str << sdisp(id);
}

// function to return an std::string of the ascii/hex version of the given 'id'.
inline std::string toHexString(const big_id& id)
{
  char ascii[big_id::id_size*2 + 1];
  priv::to_ascii_hex(ascii, id);
  return &ascii[0];
}

// template function to fill a big_id by reading 64 ascii/hex bytes from the given stream.
template<typename T>
inline T& operator>>(T& str, big_id& id)
{
  for (int i = 0; i < big_id::id_size; i++) {
    char nib1, nib2;
    str >> nib1;
    str >> nib2;
    id.m_buf[i] = (priv::hex_to_i(nib1) << 4) + priv::hex_to_i(nib2);
  }
  return str;
}

// function to return a big_id whose value is initialized from the given 'str' (must be 64 byte ascii/hex).
inline big_id hexStringId(const std::string& str)
{
  big_id id;
  const char* ss = str.c_str();
  for (int i = 0; i < big_id::id_size; i++) {
    char nib1 = *ss++;
    char nib2 = *ss++;
    id.m_buf[i] = (priv::hex_to_i(nib1) << 4) + priv::hex_to_i(nib2);
  }
  return id;
}

// template function to stream out the "short display" form of a big_id.
template<typename T>
T& operator<<(T& str, const short_big_id& id)
{
  char ascii[big_id::id_size*2 + 1];
  priv::to_ascii_hex(ascii, id);
  ascii[6] = ascii[7] = ascii[8] = '.';
  ascii[9] = 0;
  return str << &ascii[0];
}


