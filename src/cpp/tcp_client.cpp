
#include "tcp_client.h"
#include "tcp_server.h"
#include "dns_cache.h"

namespace tcp_client
{

struct tcpc_deleter
{
  tcpc_deleter(tcp_caller* tcpc)
    : tcpc_(tcpc)
  {}
  
  ~tcpc_deleter()
  {
    delete tcpc_;
  }
  
  tcp_caller* tcpc_;
};

static void inform(tcp_caller* tcpc, int err_code)
{
  tcpc->exec(err_code, std::unique_ptr<fiber_pipe>());
}

void do_connect_and_run(const char* host, int port, tcp_caller* tcpc, size_t stack_size)
{
  dns_cache::lookup_and_run(host, port, [tcpc](int err_code, const struct sockaddr *addr, socklen_t addrlen){
  
    tcpc_deleter td(tcpc);
    
    if (err_code != 0)
       inform(tcpc, err_code);
    else {
    
      int fd = socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (fd == -1) {
        inform(tcpc, errno);
        do_error("socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)");
      }
      
      #if defined(ANON_LOG_DNS_LOOKUP)
      anon_log("initiating async connect() to " << *(struct sockaddr_storage*)addr);
      #endif
      
      std::unique_ptr<fiber_pipe> pipe(new fiber_pipe(fd, fiber_pipe::network));
      auto cr = connect(fd, addr, addrlen);
      
      if (cr == 0) {
      
        anon_log("a little weird, but ok.  non-blocking connect succeeded immediately");
        
      } else if (errno != EINPROGRESS) {
      
        inform(tcpc, errno);
        do_error("connect(fd, addr, addrlen)");
        
      } else {
      
        // fiber-sleep until connect completes
        io_params::sleep_cur_until_write_possible(pipe.get());

        // did connect succeed or fail?
        int result;
        socklen_t optlen = sizeof(result);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen) != 0) {
          inform(tcpc, errno);
          do_error("getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen)");
        }
        
        if (result != 0) {
        
          #if defined(ANON_LOG_DNS_LOOKUP)
          anon_log("async connect() completed with error: " << (result > 0 ? error_string(result) : gai_strerror(result)));
          #endif

          inform(tcpc, result);
          return;
        }
        
      }
      
      // connect succeeded, call the functor
      tcpc->exec(0, std::move(pipe));
    }

  }, stack_size);
}

}

