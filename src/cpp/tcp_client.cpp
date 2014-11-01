
#include "tcp_client.h"
#include <netdb.h>


namespace tcp_client
{

struct resolve_info
{
  resolve_info(const char* host, int port, tcp_caller* tcpc, int stack_size)
    : host_(host),
      tcpc_(tcpc),
      stack_size_(stack_size)
  {
    sprintf(portString_,"%d",port);
    
    memset(&hints_, 0, sizeof(hints_));
    hints_.ai_family = AF_UNSPEC;  // use IPv4 or IPv6, whichever
    hints_.ai_socktype = SOCK_STREAM;

    // what we are trying to look up,
    // where to store the result...
    cb_.ar_name = host_.c_str();
    cb_.ar_service = &portString_[0];
    cb_.ar_request = &hints_;
    cb_.ar_result = &result_;
    
    cba_ = &cb_;

    // how to notify us when the lookup completes
    memset(&se_,0,sizeof(se_));
    se_.sigev_notify = SIGEV_THREAD;
    se_.sigev_value.sival_ptr = this;
    se_.sigev_notify_function = &resolve_info::resolve_complete;

    // start the async getaddrinfo lookup
    if (getaddrinfo_a(GAI_NOWAIT, &cba_, 1, &se_) != 0) {
      delete tcpc;
      do_error("getaddrinfo_a(GAI_NOWAIT, &cba_, 1, &se_)");
    }
  }
  
  // "callback" that the linux kernel calls when getaddrinfo_a
  // completes.  Note that this is running on a somewhat unspecified
  // thread, but it is not one of our io dispatch threads.
  static void resolve_complete(union sigval sv)
  {
    auto ths = (resolve_info*)sv.sival_ptr;
    
    int ret = gai_error(&ths->cb_);
    if (ret == EAI_INPROGRESS)
      anon_log("strange call to resolve_info::resolve_complete with gai_info returning EAI_INPROGRESS");
    else if (ret != 0) {
    
      anon_log("dns lookup for \"" << ths->host_.c_str() << "\" failed with error: " << error_string(ret));
      delete ths->tcpc_;
      delete ths;
      
    } else {
    
      auto result = ths->result_;
      auto tcpc = ths->tcpc_;
      
      // note that since we are not on a fiber at this point
      // it is possible for run_in_fiber to block attempting to write
      // to the io_dispatch command pipe if it is full.  But this
      // _should_ be, at worst a temporary block since this thread
      // should not be able to block the processing of any of the
      // io threads.
      fiber::run_in_fiber([result,tcpc]{
      
        int fd = socket(result.ai_family, result.ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, result.ai_protocol);
        if (fd == -1) {
          tcpc->exec(false, errno, std::unique_ptr<fiber_pipe>());
          delete tcpc;
          do_error("socket(result.ai_family, result.ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, result.ai_protocol)");
        }
        
        std::unique_ptr<fiber_pipe> pipe(new fiber_pipe(fd, fiber_pipe::network));
        
        if (connect(fd, result.ai_addr, result.ai_addrlen) == 0) {
        
          anon_log("a little weird, but ok.  non-blocking connect succeeded immediately");
          
        } else if (errno != EINPROGRESS) {
        
          tcpc->exec(false, errno, std::unique_ptr<fiber_pipe>());
          delete tcpc;
          do_error("connect(fd, result.ai_addr, result.ai_addrlen)");
          
        } else {
        
          // fiber-sleep until connect completes
          io_params::sleep_cur_until_write_possible(pipe.get());

          // did connect succeed or fail?
          int result;
          socklen_t optlen = sizeof(result);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen) != 0) {
            tcpc->exec(false,errno,std::unique_ptr<fiber_pipe>());
            delete tcpc;
            do_error("getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen)");
          }
          
          if (result != 0) {
            tcpc->exec(false,result,std::unique_ptr<fiber_pipe>());
            delete tcpc;
            anon_log("async connect() failed with error: " << error_string(result));
            return;
          }
          
        }
        
        // connect succeeded, call the functor
        tcpc->exec(true, 0, std::move(pipe));
        delete tcpc;
        
      }, ths->stack_size_);
      
      delete ths;
    }
  }
  
  std::string     host_;
  struct addrinfo hints_;
  struct addrinfo result_;
  struct gaicb    cb_;
  struct gaicb*   cba_;
  struct sigevent se_;
  tcp_caller*     tcpc_;
  char            portString_[8];
  int             stack_size_;
};

void do_connect_and_run(const char* host, int port, tcp_caller* tcpc, int stack_size)
{
  new resolve_info(host, port, tcpc, stack_size);
}

}

