
#include "tcp_client.h"
#include "tcp_server.h"
#include <netdb.h>
#include <pthread.h>

namespace tcp_client
{

void inform_in_fiber(tcp_caller* tcpc, int err)
{
  fiber::run_in_fiber([tcpc,err]{
    try {
      tcpc->exec(err, std::unique_ptr<fiber_pipe>());
    }
    catch(...)
    {}
    delete tcpc;
  });
}

struct notify_complete
{
  notify_complete(const char* host, tcp_caller* tcpc, size_t stack_size)
    : host_(host),
      tcpc_(tcpc),
      stack_size_(stack_size)
  {}
  
  std::string host_;
  tcp_caller* tcpc_;
  size_t stack_size_;
  struct gaicb cb_;
};


// "callback" that the linux kernel calls when getaddrinfo_a
// completes.  Note that this is running on a somewhat unspecified
// thread, but it is not one of our io dispatch threads.  In Ubuntu
// 14.04 it creates a new thread for each call to this function.
// You can see this by running in gdb.  It actually creates two
// new threads for each call...
static void resolve_complete(union sigval sv)
{
  // we set this when we called getaddrinfo_a
  // so read it back here
  auto nc = (notify_complete*)sv.sival_ptr;
  
  // why were we called back?
  int ret = gai_error(&nc->cb_);
  if (ret == EAI_INPROGRESS) {
  
    anon_log("strange call to resolve_complete with gai_error returning EAI_INPROGRESS");
    return;
    
  } else if (ret != 0) {

    #if defined(ANON_LOG_DNS_LOOKUP)
    anon_log("getaddrinfo_a completed with error: " << gai_strerror(ret));
    #endif
  
    inform_in_fiber(nc->tcpc_, ret);
    
  } else {
  
    // dns lookup succeeded
  
    #if defined(ANON_LOG_DNS_LOOKUP)
    int num_returns = 0;
    auto rslt = nc->cb_.ar_result;
    while (rslt) {
      ++num_returns;
      rslt = rslt->ai_next;
    }
    anon_log("dns lookup for \"" << nc->host_.c_str() << "\" returned " << num_returns << " result" << (num_returns > 1 ? "s:" : ":"));
    rslt = nc->cb_.ar_result;
    while (rslt) {
      anon_log("  " << *(struct sockaddr_storage*)rslt->ai_addr);
      rslt = rslt->ai_next;
    }
    #endif

    auto result = nc->cb_.ar_result;
    auto tcpc = nc->tcpc_;
    
    // note that since we are not on a fiber at this point
    // it is possible for run_in_fiber to block attempting to write
    // to the io_dispatch command pipe if it is full.  But this
    // _should_ be, at worst a temporary block since this thread
    // should not be able to block the processing of any of the
    // io threads.
    fiber::run_in_fiber([result,tcpc]{
    
      int fd = socket(result->ai_family, result->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, result->ai_protocol);
      if (fd == -1) {
        inform_in_fiber(tcpc,errno);
        freeaddrinfo(result);
        do_error("socket(result->ai_family, result->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, result->ai_protocol)");
      }
      
      #if defined(ANON_LOG_DNS_LOOKUP)
      anon_log("initiating async connect() to " << *(struct sockaddr_storage*)result->ai_addr);
      #endif
      
      std::unique_ptr<fiber_pipe> pipe(new fiber_pipe(fd, fiber_pipe::network));
      auto cr = connect(fd, result->ai_addr, result->ai_addrlen);
      freeaddrinfo(result);
      
      if (cr == 0) {
      
        anon_log("a little weird, but ok.  non-blocking connect succeeded immediately");
        
      } else if (errno != EINPROGRESS) {
      
        inform_in_fiber(tcpc,errno);
        do_error("connect(fd, result->ai_addr, result->ai_addrlen)");
        
      } else {
      
        // fiber-sleep until connect completes
        io_params::sleep_cur_until_write_possible(pipe.get());

        // did connect succeed or fail?
        int result;
        socklen_t optlen = sizeof(result);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen) != 0) {
          inform_in_fiber(tcpc,errno);
          do_error("getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen)");
        }
        
        if (result != 0) {
        
          #if defined(ANON_LOG_DNS_LOOKUP)
          anon_log("async connect() completed with error: " << (result > 0 ? error_string(result) : gai_strerror(result)));
          #endif

          inform_in_fiber(tcpc,result);
          return;
        }
        
      }
      
      // connect succeeded, call the functor
      try {
        tcpc->exec(0, std::move(pipe));
      }
      catch(...)
      {}
      delete tcpc;
      
    }, nc->stack_size_);
    
  }
  
  delete nc;
}


void do_connect_and_run(const char* host, int port, tcp_caller* tcpc, int stack_size)
{
  #if defined(ANON_LOG_DNS_LOOKUP)
  anon_log("starting dns lookup for \"" << host << "\", port " << port);
  #endif
  
  // we don't need a very big stack, so set
  // the pthread stack size to the minimum
  pthread_attr_t  ptattr;
  int rslt = pthread_attr_init(&ptattr);
  if (rslt != 0) {
    anon_log_error("pthread_attr_init(&ptattr) failed with result: " << error_string(rslt));
    inform_in_fiber(tcpc,rslt);
    throw std::system_error(rslt, std::system_category());
  }
  rslt = pthread_attr_setstacksize(&ptattr, 16*1024);
  if (rslt != 0) {
    anon_log_error("pthread_attr_setstacksize(&ptattr, 16*1024) failed with result: " << error_string(rslt));
    pthread_attr_destroy(&ptattr);
    inform_in_fiber(tcpc,rslt);
    throw std::system_error(rslt, std::system_category());
  }
  
  // the info we will need when resolve_complete is called
  auto nc = new notify_complete(host, tcpc, stack_size);

  // gai takes the port parameter as a string
  char  portString[8];
  sprintf(portString, "%d", port);
  
  // the sorts of endpoints we are looking for
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;  // use IPv4 or IPv6, whichever
  hints.ai_socktype = SOCK_STREAM;

  // what we are trying to look up,
  // where to store the result...
  nc->cb_.ar_name = host;
  nc->cb_.ar_service = &portString[0];
  nc->cb_.ar_request = &hints;
  struct gaicb* cba = &nc->cb_;
  
  // how to notify us when the lookup completes
  struct sigevent se;
  memset(&se,0,sizeof(se));
  se.sigev_notify = SIGEV_THREAD;
  se.sigev_value.sival_ptr = nc;
  se.sigev_notify_function = &resolve_complete;
  se.sigev_notify_attributes = &ptattr;

  // start the async getaddrinfo lookup
  int ret = getaddrinfo_a(GAI_NOWAIT, &cba, 1, &se);
  
  pthread_attr_destroy(&ptattr);
  
  if (ret != 0) {
    anon_log_error("getaddrinfo_a(GAI_NOWAIT, &cba, 1, &se) failed with error: " << gai_strerror(ret));
    inform_in_fiber(tcpc,ret);
    delete nc;
    throw std::system_error(ret, std::system_category());
  }
}

}

