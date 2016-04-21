
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

    # grab some standard autotools stuff
    sudo apt-get install autoconf automake libtool make gcc-4.9 g++-4.9 lcov 
    # grab curl (for url de/encode) and zmq for the awesomeness
    sudo apt-get install libcurl4-openssl-dev libzmq3-dev

Build and Install
-----------------

    # dont forget submodules
    git submodule update --init --recursive
    # standard autotools:
    ./autogen.sh
    ./configure
    make test -j8
    sudo make install

Run it
------

The library comes with a standalone binary which is essentially just a server or a simulated one that tells you whether or not a given input number is prime. The aim isn't really to do any type of novel large prime computation but rather to contrive a system whose units of work are highly non-uniform in terms of their time to completion (and yes random sleeps are boring). This is a common problem in many other workflows and primes seemed like a simple way to illustrate this.

    #simulate the whole thing with 1 vs 8 workers per worker layer
    time prime_serverd 1000000 1
    time prime_serverd 1000000 8
    
    #try the sample python only server
    python py/prime_serverd.py &> /dev/null &
    server_pid=$1
    
    #hit it with ab
    ab -k -n 1000 -c 8 http://localhost:8002/is_prime?possible_prime=32416190071
    kill $server_pid
    
    #run zmq based HTTP server
    prime_serverd tcp://*:8002 &> /dev/null &
    server_pid=$!
    
    #hit it with ab
    ab -k -n 1000 -c 8 http://localhost:8002/is_prime?possible_prime=32416190071
    kill $server_pid
    
    #be semi-amazed that its an order of magnitude faster

The Point
---------

What we want is a tool that lets you build a system that is pipelined and parallelized ie. the zmq "butterfly" or "parallel pipeline" pattern. See [this tutorial](http://zeromq.org/tutorials:butterfly). We'll get to why in a bit but first, this is kind of what it should look like:

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


A client (a browser or just a separate thread) makes a request to a server. The server listens for new requests and replies when the backend bits send back results. The backend is comprised of load balancing proxies between layers of worker pools. In real life you may run these in different processes or on different machines. We only use threads in order to simulate this within a single process, so please note the lack of any mutex/locking patterns (thank you zmq!).

So this system lets you handle one type of request that can be decomposed into multiple steps. That is useful if certain steps take longer than others because you can scale them independently of one another. It doesn't really handle multiple types of requests unless workers learn more than one job. To fix this we could upgrade the workers to be able to forward work to more than one proxy. This would allow heterogeneous workflows without having to make smarter/larger pluripotent workers and therefore would allow scaling of various workflows independently of each other. An easier approach would be just running a separate cluster per workflow, pros and cons there too.

You may be asking yourself, why on earth are all of the worker pools hooked into the loopback. This has two important functions. The first one is that a request could enter an error state at any stage of the pipeline. It's important to be able to signal this back to the client as soon as possible. This basically leads to the second more general idea that, indeed, certain requests have known results (error or otherwise) without going through all stages of the pipeline.

The Impetus
-----------

So the toy example of an HTTP service that computes whether or not a number is prime illustrates, at the most simplistic level, why someone might like the setup described above. But it's not the actual use-case that drove the creation of this project. Having worked on a few service oriented archtectures my team members and I noticed that we'd gained an appreciation for a certain set features. In buzz-word form those were roughly:

* Simplicity
* Flexibility
* Fault Tolerance
* Separation of Concerns
* Throughput
* Load Balancing
* Quality of Service
* Non-blocking
* Web Scale (just kidding)

Anyway back to the real use-case.. Basically we needed to handle HTTP requests that would have (widely) varying degrees of complexity. Specifically we were writing [some software](https://github.com/valhalla) that does shortest (for some value of short) path computations over large graphs. For example, users would be able to make requests to get the best route by car/foot/bike/etc. from London to Edinburgh. This may take 10s of milliseconds. In contrast though, a user would also be able to ask for the route from Capetown to Beijing. This could take a few seconds. So there you go, you can see the inherent heterogeneity of the situation. But its worse than that! You might imagine that finding a path through a graph sounds pretty straight forward but trust me when I say, making that path useful requires a bunch of extra work. Conveniently, or perhaps by force, decomposing the problem into these steps helps you achieve some of those buzz words above but doing so in the context of an HTTP server API required a little extra consideration. The thought of (or hope for) many simultaneous users made us realize we needed to strive for most of the things noted above. Especially the last one (again just kidding).

The Path
--------

I don't know how to sugar-coat this. This was so fun to build for so many reasons. The first thing we did was prove out the zmq butterfly pattern as a tiny github gist. The idea was that we could put an HTTP server in front of the this pattern and hit some of those pesky buzz words just by having separate stages of the pipeline. Surprisingly though, actually learning this pattern and figuring out how it would work in a concrete scenario was another delight. 

It's time for a Public Service Anouncement. Have you read the zeromq [documentation]()? I am not a fan of technical writing by any stretch but it is an absolute joy to read. Warning: it may cause you to re-think many many past code design decisions. Don't worry though, that feeling of embarassment over past poor decisions is just an indicator of making better informed ones in the future! Massive kudos to Pieter Hintjens. If you are hungry for more checkout their respective blogs as well, both are fantastic reads!

Anyway, with so little setup around writing/running/debugging your code you can throw it away more easily if it doesn't work out. And throw out code we did; revsion after revision getting to the point where we were left with a parallelized pipeline whose stages had a loopback to a common entrypoint.

So then began the search for a suitable HTTP server that could interact with our pipeline. We basically scoured the internet for servers with zmq bindings. As is no surprise, there are lots of good options out there! We spent some time prototyping using a few different ones but then we stumbled upon a very interesting search result. It was a blog from Pieter Hintjens about the new ZMQ_STREAM socket type. It describes, with examples, how the socket works and ends up making a small web server using it. Hintjens goes on to say that a lot more work would be needed for a full fledged HTTP server and describes some of the missing parts in a bit more detail.

The idea was enticing, could we build a minimal HTTP server with just zmq to sit in front of our pipeline. We threw some stuff into a gist once again and started testing. Before long and with very little code, we had something! From there though it was on to writing an HTTP state machine to handle the streaming nature of the socket type. Writing state machines, especially against a couple of protocol versions at the same time is torture in terms code re-use. But we'll get to that in future work section.


The API
-------

TODO: describe the bits of the API so people have more than example programs to figure out how to use it

The Future
----------

The first thing we should do make use of a proper HTTP parser. There are some impressive ones out there, notibly the one used in one of the webservers we tested (H2O) called PicoHTTPParser. There may be a few issues with the 

The second thing we want to do is work zbeacon perks into the API. Currently the setup of a pipeline is somewhat cumbersome, each stage must know about the previous and next stages as well as the loopback. Then we also complicate things by making the next stage optional since the pipeline isn't infinite. It's clunky and requires decent understanding to get it right. It's also very manual. With zbeacon, applications can broadcast their endpoints to peers so that they can connect to eachother through discovery rather than via manual configuration. This is pretty great in itself but thats not the really interesting part here. What if our pipeline weren't a pipeline? What if it were a graph?!

    client <---> server ----------
                 ^ | ^            \
                 | | |             v
                 | | |       =============
                 | | |   --> |   proxy   | <----
                 | | |  /    |-----------|      \
                 | | |  \    |  workers  |      |
                 | | |   --- |    ...    | ---  |
                 | | |       =============    \ |
                 | | \            /           | |
                 | \  ------------            | |
                 |  --------------            | |
                 |                \           | |
                 |                 v          | |
                 |           =============    / |
                 |       --> |   proxy   | <--  |
                 |      /    |-----------|      |
                 |      \    |  workers  |      /
                 |       --- |    ...    | -----
                 |           =============
                 \                 /
                  -----------------

Ok we may be reaching the limits of ascii 'art' here but bear with me... The implications are huge! We can remove the shackles of a the rigid fixed-order pipeline. Instead we can have application code determine the stages a request is forwarded to on the fly. For example, imagine you have a request that requires a bunch of iterations of a specific stage in the traditional pipeline. Essentially the only option you have is to perform the iterations on a single worker basically locking that worker until all the iterations are done. This would seem unfair, a certain request can ask for n iterations worth of work while the next request may only need one. Allowing the stages to be connected in a graph structure (including cycles) would give the application the option to load balance portions of a larger request until the entire request has been fulfilled. Indeed any problem that could be broken down into tasks of equal size (or at least more equal size) would have the potential to handle requests in a much more fair fashion.

A graph structure for the various stages also has the potential to better organize the functionalities of worker pools.






















