
#pragma once

#include "big_id.h"

bool init_big_id_crypto(); // call this prior to any of the crypto functions, returns true on success, false on failure.
void term_big_id_crypto(); // call this at the end of a process's lifetime to remove any resources allocated by init_big_id_crypto().

// Function to return a big_id whose value is random (with crypto strengh randomness).
big_id rand_id();

// Function to return a big_id whose value is the SHA256 checksum value of the given 'buf'.
big_id sha256_id( const char* buf, size_t len );

