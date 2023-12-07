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

#include <cinttypes>

inline uint16_t get_be_uint16(const uint8_t *ptr)
{
  uint16_t val = ptr[0];
  val <<= 8;
  return val + ptr[1];
}

inline void set_be_uint16(uint16_t val, uint8_t *ptr)
{
  ptr[0] = (val >> 8) & 0x00ff;
  ptr[1] = val & 0x00ff;
}

inline uint32_t get_be_uint32(const uint8_t *ptr)
{
  uint32_t val = ptr[0];
  val <<= 8;
  val += ptr[1];
  val <<= 8;
  val += ptr[2];
  val <<= 8;
  return val + ptr[3];
}

inline void set_be_uint32(uint32_t val, uint8_t *ptr)
{
  ptr[0] = (val >> 24) & 0x00ff;
  ptr[1] = (val >> 16) & 0x00ff;
  ptr[2] = (val >> 8) & 0x00ff;
  ptr[3] = val & 0x00ff;
}

inline uint64_t get_be_uint64(const uint8_t *ptr)
{
  uint64_t val = 0;
  for (auto i = 0; i < 8; i++) {
    val <<= 8;
    val += ptr[i];
  }
  return val;
}

inline void set_be_uint64(uint64_t val, uint8_t* ptr)
{
  for (auto i = 0; i < 8; i++) {
    ptr[i] = (val >> (56 - i*8)) & 0x0ff;
  }
}
