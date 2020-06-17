# dist_id_shore
A prototype distributed unique-ID generator.

Problem Summary
===============

Create an Identity Service that generates guaranteed\* globally unique 64-bit identifiers.
There will be up to 1024 nodes (with static node-ID's fromn 0 to 1023).
The nodes will have up to 100k ID-generation QPS (requests-per-second).

You are given a functions that return:
   * the node-ID, and
   * a millisecond timestamp (origin at the epoch)

Preference given to correctness, simplicity, robustness, and minimal coordination/storage.

Note, the problem statement says that node\_id() and timestamp() don't need implementations, 
but then asks for definite statemens about guarantees, testing, and performance. all of which 
hinge heavily on the **actual** implemention of at least 'timestamp()'.


Theoretical Analysis
====================

Assuming that we have a truly stable node-ID\*, and consistent reliable clocks\* on each node,
then there is a trivial solution...

First, note that millisecond resolution only allows 1000 requests-per-second.
Since we need 100x that, we will need to subdivide the ID-space further.

For simplicity (and room for performance expansion), assume that we split the ID-space as follows:

   * 10 bits for the node-ID
   * 10 bits for a sub-millisecond counter
   * 44 bits for the timestamp

This allows for approximately 1M IDs per-node per second (1024 x 1024).
This also allows for timestamps some 557 years (from the epoch) range (e.g. 2<sup>44</sup> / (1000\*60\*60\*24\*365) ).

Why does this work\* ?
Partitioning the keyspace with the node-ID means that we don't have to worry about collisions between nodes.
So, now we can restrict our analysis to IDs from a single node...

We control the sub-millisecond counter, but it will wrap-around at 1024 IDs. So we need to make sure that 
it doesn't wrap around in the span of a single millisecond. Given the 100K RPS limit, it will not.
We can also enforce this, by inserting delay of up to the next millisecond whenever the counter wraps before a full millisecond.

Now, assuming that the clock is an ideal\* clock, always running forward 1 msec / msec, then the timestamp 
part of the ID will only repeat when the counter value is different, and we will sucessfully generate 
guaranteed\* unique IDs for up to 557 years.

\* - Offer not valid in California, Alaska, Romania, Russia, New York, or Hawaii.
Which leads us to...


Practical Analysis
==================

In practice, some node(s) will be started via automated system or manual typo with duplicate node-ID.
And of course nodes will be mercilessly killed by Kubernetes because some other pod on the same node 
has used an abundance of IO/CPU/Memory. But that's the least of our worries...

Anyone who's used clocks in a real production environment knows that the above assumptions are wildly optimistic 
("What do you mean the SSL Cert is 'expired'?. Oh... the clock on this server is off by a full **decade**."
 "Why is my adaptive throttling/rate-limiting algorithm exploding? Oh... the clock went **backwards**.").
In practice, clocks are unreliable for a number of reasons:
   * Time-warps from the normal functioning of NTP
   * NTP or the network bouncing up and down like a pogo stick
   * Leap-seconds
   * Daylight Savings
   * Local vs UTC confusion
   * Misconfiguration
   * A whole laundry-list of problems between VMs/containers and the host clock, network, etc.

Clock correctness will scale with dollars, but in a highly nonlinear fashion. 
For example, Google Spanner solved this problem with atomic clocks and GPS for the 
[TrueTime API](https://static.googleusercontent.com/media/research.google.com/en//pubs/archive/45855.pdf),
but they still provide an interval, instead of a specific single value.

However, the problem statement does not actually require perfectly accurate clocks. Since we only need 
unique identifiers (vs total-ordering of IDs), we only require a monotonic clock, so that values never repeat. 
So of all the problems listed above, we *should* only need to worry about NTP time-warps, DST rolling back, 
and misconfiguration.

The linux/unix kernel (and c-libraries) provides a few timestamp options:
   1) clock\_gettime() has two monotonic time options, but they have an arbitrary orign.
   2) gettimeofday() has an epoch origin, but suffers from warping
   3) processor TSC registers may be monotonic (depending on CPU version and configuration), but arbitrary origin.

Note: [the Erlang time docs](https://erlang.org/doc/apps/erts/time_correction.html) have the same limitations,
and are almost certainly calling clock\_gettime() and gettimeofday().

The arbitrary origin means that clocks might repeat on reboot, or migration of the service to another 
physical node/VM/container. So node restarts or migration will violate the uniqueness guarantee.
Warping means that arbitrary time-spans will repeat, so normal operation of a node will violate ID uniqueness.

So, by themself, none of these clocks will be suitable. However, if we maintain a high-water mark (per node-ID),
then we can maintain a delta from the high-water mark to the monotonic clock and generate a monotonic, 
vaguely-realtime clock. When there is no pre-existing high-water mark, we use the warpable gettimeofday() 
to calculate the delta. That, in combination with lazy/conservative updating of timestamps, and startup delay 
of service-nodes should help minimize the likelyhood of ID collisions in practice.


Other Practical Considerations
==============================

Since VMs and containers are oftem migrated with no persistent storage, the high-water mark for each node 
would ideally be persisted somehow. This could be via a database, coordination service (e.g. ZooKeeper),
or various other means.
Since the problem statement prioritizes simplicity, robustness, and minimal storage, and since the data
that needs to be stored is small (maybe 20 bytes per node-ID), those solutions are rather heavy-weight.
Instead, we can borrow a page from ZeroConf, Rendevouz and older systems, and use multicast to advertise 
(and query) nodes and their high-water mark. This also has the advantage of easily identifying duplicate
node-IDs (if we wait and listen for some interval at startup). 
For simplicity, we can redundantly store the high-water mark on all peer nodes. But we could also 
run the high-water server(s) independently for backup.
Note: In the event of a network partition, duplicate node-IDs are possible, placing this system on 
the AP side of the CAP theorem. If you prefer the Consistency side, then approaches like ZooKeeper,
or a central server that leases out time intervals (e.g. second per node-ID) to nodes would be better.

Also, timestamp functions can be relatively slow, but there is no need to fetch timestamps on every 
ID request. We can delay calling 'timestamp()' until the counter wraps, and we could use more bits 
for the counter, and less for the timestamp (i.e. second resolution, and 20 bits for the counter).
This allows us to update the high-water mark less often (lower network traffic and file writes).
Note: Also, we only need increment the the timestamp by 1 (vs all the way to the current time) on 
counter-wrapping, which further lowers the odds of collisions on node migration, and allows for 
request bursts. 
If requests burst higher than provisioned, then we sleep a short interval before returning an ID.

Of course, this code is also a rough prototype. A real system would want more care and polish.

Discussion Notes:
=================

Performance:
------------
Performance for a C++ implementation was measured at 2.4 sec for generating one million timestamps,
on a Intel i5 (7th gen) laptop.
Note: As noted above, this could be improved by increasing the counter size (there's a forced wait 
when the counter "wraps").  But this would also require stealing bits in the ID from the timestamp.
Of course, if the individual ID requests are going over the network, performance will be lower.

Correctness:
------------
Node uniqueness is handled in both the single-node or entire-system crash/restart cases by multicast 
announcements of node-ids and high-water timestamps, both at startup and at regular intervals.
(Yes, UDP can lose packets, but this can be parts-per-billion on properly configured/provisioned networks).
Currently, it only checks that the ports are the same (because the sockets don't bind to a specific interface).
Enumerating interfaces and picking one, or adding other discriminators (PID, process start-time, GUID) would 
improve this (though for standardized container images, the PID might not be different).

There is a tradeoff between startup delay and collision probabilities. Heavily loaded networks can 
experience packet delays in the 10s of seconds, during which time, a migrated node with a slow clock
could be re-using IDs.

Software Defects:
-----------------
How to handle software defects?: test, test test...
For the core algorithm, we should verify that different node-ids, counter values, and timestamps result in 
different returned IDs (and that multiple calls with the same values don't).

For the running system on a single node, the id's are strictly monotonic (as long as the counter bits come 
after the timestamp bits). So we can verify that each successive ID value is numerically greater than the 
last (for a given node).

Of course boundary conditions need to be tested, ranges verified, and a host of other checks.


Building, Running, Testing:
===========================

The program should\* run on any linux system with a relatively recent g++ compiler (and some other Unix systems with minor tweaks to the network code). It has no external dependencies.

To build, just run ```make```.
To test, run ```make check```.
If you have valgrind installed, you can check for memory leaks, etc. with ```make memcheck```.

Building produces a ```client``` executable, which takes a node-id, and an optional count (default 1,000,000).
The client dumps the generated IDs in hex format to stdout.


