
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



