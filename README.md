Anon
====

![It Goes To 11!](http://beerpulse.com/wp-content/uploads/2011/08/BellsGoesTo11Front.png?raw=true)

Anon is an experiment in server design for "Services".  It attempts to achieve
several goals.  These are:

* Maximize efficiency on a single CPU instance
* Effeciently deal with "over-maximum" requests
* Provide a reasonable dev environment

Anon is currenty HIGHLY experimental and is unlike other service designs in many
ways.  For example, it is currently all written in C++.  The name "anon" comes
from the first two goals, and is the word *anon*, meaning something like "again".
It's not an abreviation for "anonymous".

A common problem in some server designs is one that I'll call the "infinite queue"
problem.  It can be seen when the Service design has components that look something
like the following -- shown in C++, but it can exist in any language:

```C++
// some global
std::deque<int> g_new_connections;

// runs in one thread
void new_connections_loop(int listening_socket)
{
  while (true) {
    int new_connection = accept(listening_socket, 0, 0);
    g_new_connections.push_back(new_connection);
  }
}

// runs in another thread
void process_connections_loop()
{
  while (true) {
    int conn = g_new_connections.front();
    g_new_connections.pop_front();
    process_one_connection(conn);
  }
}
```

This code isn't meant to be fully correct, it's missing mutex's and stuff.
But it shows a central feature of many server designs where there exists some
kind of queue, here shows as the std::deque `g_new_connections`, where one thread
of execution accepts new server connections as fast as it can and puts each
one on a queue.  Another thread of execution pulls items off the of the queue
and processes them as fast as it can.  The basic idea shown here is that there
isn't a good way for the second thread to keep the first from getting too far
ahead of it.  If the `process_one_connection` function takes a long time to
complete relative to the speed at which other machines are trying to establish
new connections to this one, then the `g_new_connections` queue can grow
arbitrarily large.  The root cause of many Critical Service Outages, particularly
when they occur due to excessive server load, can be traced to a basic problem
where a consumer of queued requests falls behind the producer of them.  When
that starts to happen the percentage of the total Service's compute and resource
capacity that is dedicated to maintaining the queue itself grows, further
slowing down `process_one_connection`, which then componds the problem.

Linux has an errno code named EAGAIN which it uses when certain operations
that are requested to be non-blocking are not currently possible for one reason
or another.  A common use of EAGAIN would be to set `listening_socket` above to
be non-blocking, and then the call to `accept` would return -1 and set errno
to EAGAIN if there were not any connections that could be returned when the
code called `accept`.   That kind of usage would allow that thread of execution
to go do something else instead of stay stuck in `accept` until someone tries
to connect to our computer.

But EAGAIN can also be used on the write-side of an operation.  If
`g_new_connections` were a pipe of some kind instead of the deque that is
shown, then the `push_back` would be some kind of `write` call.  The `front`
and `pop_front` would then be replaced by `read` calls.  In that kind of design
the pipe could be set non-blocking, and since it has a finite internal size,
if `new_connections_loop` gets too far in front of `process_connections_loop`
the `write` call will fail with errno set to EAGAIN.  And this can serve as
a very natural way for a consumer of requests to signal to the producer that
it needs to slow down.

Even without setting the pipe to be non-blocking, using a pipe with small-ish,
finite capacity will cause `new_connections_loop` to *block* inside of its
`write` call, which will keep it being able to call `accept` again, which
then keeps client machines from being able to connect and send new requests.
This can create a kind of "back pressure" that `process_connections_loop` can
assert on the entire Service.  But having client machines fail to connect
without any understanding of why makes it hard to get those client machines
working correctly.  So a basic principle of Anon is to propogate the EAGAIN
concept through the entire Service.

In the Anon design `new_connections_loop` does use a non-blocking pipe, and
so sees that it has gotten too far ahead of `process_connections_loop` (because
it sees errno as EAGAIN) and can then enter a state where further accept calls
are immediately replied with a kind of EAGAIN message and then shut down.  That
distributes the EAGAIN processing throughout the entire service -- thus the name
Anon for this project.

Conveniently, EAGAIN is errno 11, letting me tie the name to the other goal
of a server design that is as efficient as the machine allows.  A second piece
of Anon is to provide a design that makes good use of Linux's event dispatching
mechanism "epoll" and then provide a platform where all request processing
can be done free of any blocking operations.  In fact, the goal is to allow Anon
servers to run in a model where the number of os threads running is equal to the
number of CPU cores.  In this model, each request is handled by user-level threads
(fibers) and fiber scheduling is driven by Linux's epoll event dispatching mechanism.


