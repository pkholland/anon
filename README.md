anon
====

Experiments in distributed queueing

![It Goes To 11!](http://beerpulse.com/wp-content/uploads/2011/08/BellsGoesTo11Front.png?raw=true)

Anon is an experiment in server design for "Services".  It attempts to achieve
several goals.  These are:

* Maximize efficiency on a single CPU instance
* Effeciently deal with "over-maximum" requests
* Provide a reasonable dev environment

Anon is currenty HIGHLY experimental and is unlike other service designs in many
ways.  For example, it is currently all written in C++.  The name "anon" comes
from the first two goals, and is the word "anon", meaning something like "again",
not an abreviation for "anonymous".

A common problem in some server designs is one that I'll call the "infinite queue"
problem.  It can be seen when the server design has components that look something
like the following -- shown in C++, but it can exist in any language:

```C++
// runs in one thread
void new_connections_loop(int the_accept_socket)
{
  while (true) {
    int new_connection = accept(the_accept_socket, 0, 0);
    g_new_connections.push_back(new_connections);
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
But it shows a central feature of many server designs where there exists
some kind of queue, here shows as deque `g_new_connections` where one thread
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
slowing down `process_one_connection` which then componds the problem.
    
