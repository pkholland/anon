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

#include <stdint.h>
#include <string.h>


struct big_id
{
    big_id() {}  // leaves data uninitialized
    big_id( const uint8_t (&buf)[32] ) { memcpy( &m_buf[0], &buf[0], id_size ); }    // copies the given 'buf'

    enum { id_size = 32 };
    uint8_t m_buf[id_size]; // storage
};

/** helper functions to make it easier to implement collections of big_id's */

// Same
inline bool operator==( const big_id& id1, const big_id& id2 ) { return ::memcmp( &id1.m_buf[0], &id2.m_buf[0], big_id::id_size ) == 0; }

// Not the same
inline bool operator!=( const big_id& id1, const big_id& id2 ) { return ::memcmp( &id1.m_buf[0], &id2.m_buf[0], big_id::id_size ) != 0; }

// Less than, strictly for usage as a key in structures like std::map
inline bool operator< ( const big_id& id1, const big_id& id2 ) { return ::memcmp( &id1.m_buf[0], &id2.m_buf[0], big_id::id_size ) <  0; }



