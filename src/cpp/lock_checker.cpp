
#if defined(ANON_RUNTIME_CHECKS)

#include "lock_checker.h"
#include "log.h"

namespace anon
{

static thread_local int  lock_count;

void inc_lock_count()
{
  ++lock_count;
}

void dec_lock_count()
{
  --lock_count;
}

void assert_no_locks()
{
  if (lock_count)
    do_error("assert_no_locks() called with " << lock_count << " lock" << (lock_count > 1 ? "s" : ""));
}

}

#endif

