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

#include "fiber.h"
#include <netinet/in.h>

namespace dns_lookup
{

void start_service();
void end_service();

// stall this fiber and initiate an async dns lookup,
// once that dns lookup completes resume this fiber
// and return the looked up information.  .first is the
// err_code.  If this is zero then .second is one or
// more sockaddrs returned by the dns lookup.  They are
// stored as inet6 addrs, but can be either normal inet(4)
// or inet6.  You can tell by looking at sa6_family field.
// If .first != 0, then an error has occured and .second
// is empty.  Error's less than 0 indicate gai errors.
// errors greater than 0 indicate errno errors.
std::pair<int, std::vector<sockaddr_in6>> get_addrinfo(const char *host, int port);
} // namespace dns_lookup
