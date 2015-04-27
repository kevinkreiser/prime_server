Grab some deps
--------------

    # grab the latest zmq library:
    git clone https://github.com/zeromq/libzmq.git
    cd libzmq
    ./autogen.sh
    ./configure --without-libsodium
    make -j8
    sudo make install

    # grab the c++ bindings:
    wget https://raw.githubusercontent.com/zeromq/cppzmq/master/zmq.hpp -O /usr/local/include/zmq.hpp

Build and Install
-----------------

    #standard autotools:
    ./autogen.sh
    ./configure
    make test -j8
    sudo make install

Run it
------

The library comes with a standalone binary which is essentially just a server or a simulated one that tells you whether or not a given input number is prime. The aim isn't really to do any type of novel large prime computation but rather to contrive a system whose units of work are highly non-uniform in terms of their time to completion (and yes random sleeps are boring). This is a common problem in many other workflows and primes seemed like a good way to illustrate this.

    #simulate the whole thing
    time simulator 10000000 1
    time simulator 10000000 8

    #actually run a server with with a backend
    for c in 1 8; do
        backend $c &
        BACK="$!"
        python server.py &
        SERVE="$!"
        #TODO: time xargs requests or something..
        kill $SERVE
        kill $BACK
    done

The Point
---------

What we want is a tool that lets you build a system that is pipelined and parallelized ie. the zmq "butterfly" or "parallel pipeline" pattern. See http://zeromq.org/tutorials:butterfly. Should end up looking something like:

      (server)        request_producer
                      /      |       \
                     /       |        \
    (parallelize)  worker  worker   worker ...
                     \       |        /
                      \      |       /
     (rebalance)    intermediate_router
                      /      |       \
                     /       |        \
    (parallelize)  worker  worker   worker ...
                     \       |        /
                      \      |       /
       (reply)       response_collector

This seems pretty decent for some offline scientific code that just pumps jobs into the system and waits for the results to land at the bottom. However this isn't very useful for online systems that face users for example. For that we need some kind of loopback so that we can get the result back to the requester. Something like:

                                         ==========                   ==========
                                         | worker |                   | worker |
                                         | worker |                   | worker |
    client <---> server ---> proxy <---> |  ....  | <---> proxy <---> |  ....  | <---> ....
                   ^                     | worker |                   | worker |
                   |                     | worker |                   | worker |
                   |                     ==========                   ==========
                   |                         |                            |
                   |_________________________|____________________________|___________ ....


A client (a browser or just a thread within this process) makes request to a server. The server listens for new requests and replies when the backend bits send back results. The backend is comprised of load balancing proxies between layers of worker pools. In real life you may run these in different processes or on different machines. We only using threads in order to simulate this, so please note the lack of classic mutex patterns (thank you zmq!).

So this system lets you handle one type of request that can decomposed into multiple steps. That is useful if certain steps take longer than others because you can scale them individually. It doesn't really handle multiple types of requests unless workers learn more than one job. To fix this we could upgrade the workers to be able to forward work to more than one proxy. This would allow heterogeneous workflows without having to make smarter/larger pluripotent workers and therefore would allow scaling of various workflows independently of each other. An easier approach would be just running a separate cluster per workflow, pros and cons there too.
