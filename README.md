
             o                                              
    .,-. .--..  .--.--. .-.     .--. .-. .--..    ._.-. .--.
    |   )|   |  |  |  |(.-'     `--.(.-' |    \  / (.-' |   
    |`-' ' -' `-'  '  `-`--'____`--' `--''     `'   `--''   
    |                                                       
    '                                                       

Build Status
------------

[![Build Status](https://travis-ci.org/kevinkreiser/prime_server.svg?branch=master)](https://travis-ci.org/kevinkreiser/prime_server)

Grab some deps
--------------

    # grab the latest zmq library:
    git clone https://github.com/zeromq/libzmq.git
    cd libzmq
    ./autogen.sh
    ./configure --without-libsodium
    make -j8
    sudo make install

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

    #simulate the whole thing with 1 vs 8 workers per worker layer
    time prime_serverd 1000000 1
    time prime_serverd 1000000 8
    
    #try the sample python only server
    python py/prime_serverd.py &> /dev/null &
    server_pid=$1
    
    #hit it with ab
    ab -k -n 1000 -c 8 http://localhost:8002/is_prime?possible_prime=32416190071
    kill $server_pid
    
    #run zmq based http server
    prime_serverd tcp://*:8002 &> /dev/null &
    server_pid=$!
    
    #hit it with ab
    ab -k -n 1000 -c 8 http://localhost:8002/is_prime?possible_prime=32416190071
    kill $server_pid
    
    #be semi-amazed that its close to 100x faster

The Point
---------

What we want is a tool that lets you build a system that is pipelined and parallelized ie. the zmq "butterfly" or "parallel pipeline" pattern. See http://zeromq.org/tutorials:butterfly. Should end up looking something like:

                           (input)        request_producer
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
                          (output)       response_collector

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


A client (a browser or just a thread within this process) makes a request to a server. The server listens for new requests and replies when the backend bits send back results. The backend is comprised of load balancing proxies between layers of worker pools. In real life you may run these in different processes or on different machines. We only using threads in order to simulate this, so please note the lack of classic mutex patterns (thank you zmq!).

So this system lets you handle one type of request that can decomposed into multiple steps. That is useful if certain steps take longer than others because you can scale them individually. It doesn't really handle multiple types of requests unless workers learn more than one job. To fix this we could upgrade the workers to be able to forward work to more than one proxy. This would allow heterogeneous workflows without having to make smarter/larger pluripotent workers and therefore would allow scaling of various workflows independently of each other. An easier approach would be just running a separate cluster per workflow, pros and cons there too.

You may be asking yourself, why on earth are all the worker pools hooked into the loopback. This has two important functions. The first one is that a request could enter and error state at any stage of the pipeline, its important to be able to signal this back to the client as soon as possible. This basically leads to the second more general idea that, indeed, certain requests have known results (error or otherwise) without going through all stages of the pipeline.