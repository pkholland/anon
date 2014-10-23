
#pragma once

class src_addr_validator
{
public:
  bool is_valid(const struct sockaddr_storage *sockaddr, socklen_t sockaddr_len) const
  {
    return true;
  }
};


