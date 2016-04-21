
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

What we want is a tool that lets you build a system that is pipelined and parallelized ie. the ZMQ "butterfly" or "parallel pipeline" pattern. See [this tutorial](http://zeromq.org/tutorials:butterfly). We'll get to why in a bit but first, this is kind of what it should look like:

                           (input)        request_producer
                                          /      |       \
                                         /       |        \
                                        v        v         v
                        (parallelize)  worker  worker   worker ...
                                         \       |        /
                                          \      |       /
                                           v     v      v
                         (rebalance)    intermediate_router
                                          /      |       \
                                         /       |        \
                                        v        v         v
                        (parallelize)  worker  worker   worker ...
                                         \       |        /
                                          \      |       /
                                           v     v      v
                          (output)       response_collector

This seems like a pretty good pattern for some offline scientific code that just pumps jobs into the system and waits for the results to land at the bottom. However this isn't very useful for online systems that face users for example. For that we need some kind of loopback so that we can get the result back to the requester. Something like:

                                         ==========                   ==========
                                         | worker |                   | worker |
                                         | worker |                   | worker |
    client <---> server ---> proxy <---> |  ....  | <---> proxy <---> |  ....  | <---> ....
                   ^                     | worker |                   | worker |
                   |                     | worker |                   | worker |
                   |                     ==========                   ==========
                   |                         |                            |
                   \_________________________|____________________________|___________ ....


A client (a browser or just a separate thread) makes a request to a server. The server listens for new requests and replies when the backend parts send back results. The backend is comprised of load balancing proxies between layers of worker pools. In real life you may run these in different processes or on different machines. We use threads in the example server to conveniently simulate this within a single process, so please note the lack of any mutex/locking patterns (thank you ZMQ!).

This system lets you handle types of requests that can be decomposed into a single pipeline of multiple steps, which is useful if certain steps take longer than others because you can scale them independently of one another. It doesn't really handle types of requests which would need multiple pipelines, or branching pipelines, unless workers can do more than one job. To fix this we could upgrade the workers to be able to forward work to more than one proxy. This would allow heterogeneous workflows without having to make smarter/larger pluripotent workers and therefore would allow scaling of various workflows independently of each other. An easier approach would be just running a separate cluster per workflow, pros and cons there too.

You may be asking yourself, why on earth are all of the worker pools hooked into the loopback? This has two important functions:

1. A request could enter an error state at any stage of the pipeline. It's important to be able to signal this back to the client as soon as possible.
2. More generally, certain requests have known results (error or otherwise) without going through all stages of the pipeline.

The Impetus
-----------

The toy example of an HTTP service that computes whether or not a number is prime is a simple illustration of why someone might want a setup as described above. But it's not the actual use-case that drove the creation of this project. Having worked on a few service oriented archtectures my team members and I noticed that we'd compiled what amounted to a wishlist of architectural features. In buzz-word form those were roughly:

* Simplicity
* Flexibility
* Fault Tolerance
* Separation of Concerns
* Throughput
* Load Balancing
* Fair Queuing
* Quality of Service
* Non-blocking
* Web Scale (just kidding)

We needed to handle HTTP requests that would have widely varying degrees of complexity. Specifically, we were writing [some software](https://github.com/valhalla) that does shortest (for some value of short) path computations over large graphs. For example, users would be able to make requests to get the best route by car/foot/bike/etc. from London to Edinburgh, which may take 10s of milliseconds. In contrast though, a user would also be able to ask for the route from Capetown to Beijing, which could take a few seconds. Different requests can vary in computation time over several orders of magnitude.

But its worse than that! You might imagine that finding a path through a graph sounds pretty straight forward but making that path useful requires a bunch of extra work. Conveniently, or sometimes through great effort, decomposing a problem into discrete steps can help you achieve some of those buzz-words above. Doing so in the context of an HTTP server API requires a little extra consideration. Thinking of (or hoping for) many simultaneous users made us realize we needed to strive for most of the buzz-words above. Especially the last one (again just kidding).

The Path
--------

This was so fun to build for so many reasons. The first thing we did was prove out the ZMQ butterfly pattern as a tiny Github gist. The idea was that we could put an HTTP server in front of the this pattern and hit some of those pesky buzz-words just by having separate stages of the pipeline. Surprisingly, learning this pattern and figuring out how it would work in a concrete scenario was another delight.

It's time for a Public Service Anouncement. Have you read the ZeroMQ [documentation](http://zguide.zeromq.org/page:all)? I am not usually a fan of technical writing, but it is an absolute joy to read. Warning: it may cause you to re-think many many past code design decisions. Don't worry though, that feeling of embarassment over past poor decisions is just an indicator of making better informed ones in the future! Massive kudos to Pieter Hintjens who wrote those docks. If you are hungry for more good writing checkout his [blog](http://hintjens.com/) or that of another ZMQ author [Martin Sústrik](http://250bpm.com/), both are fantastic reads!

Back to the gist. With so little setup around writing, running and debugging your code you can throw it away more easily if it doesn't work out. And throw out code we did; revision after revision until we got to the point where we were left with a parallelized pipeline whose stages had a loopback to a common entrypoint.

Then began the search for a suitable HTTP server that could interact with our pipeline. We scoured the internet for servers with ZMQ bindings. As is no surprise, there are lots of good options out there! We spent some time prototyping using a few different ones but then we stumbled upon a very interesting search result. It was a [blog post](http://hintjens.com/blog:42) from Pieter Hintjens about the ZMQ_STREAM socket type. It describes, with examples, how the socket works and ends up making a small web server using it. Hintjens goes on to say that a lot more work would be needed for a full fledged HTTP server and describes some of the missing parts in a bit more detail.

The idea was enticing; could we build a minimal HTTP server with just ZMQ to sit in front of our pipeline? We threw some stuff into a gist once again and started testing. Before long and with very little code, we had something! From there though it was on to writing an HTTP state machine to handle the streaming nature of the socket type. Writing state machines, especially against a couple of protocol versions at the same time is torture in terms code re-use. But we'll get to that in future work section.

The API
-------

TODO: describe the bits of the API so people have more than example programs to figure out how to use it

The Future
----------

The first thing we should do is make use of a proper HTTP parser. There are some impressive ones out there, notibly PicoHTTPParser which is used in one of the webservers we tested (H2O). There may be a few issues with the streaming nature of the ZMQ_STREAM socket but they are worth working out so as not to have to maintain the mess of code required to properly parse HTTP.

The second thing we want to do is work zbeacon perks into the API. Currently the setup of a pipeline is somewhat cumbersome, each stage must know about the previous and next stages as well as the loopback. Then we also complicate things by making the next stage optional since the pipeline isn't infinite. It's clunky and requires decent understanding to get it right. It's also very manual. With zbeacon, applications can broadcast their endpoints to peers so that they can connect to eachother through discovery rather than via manual configuration.

Automatic service discovery is pretty great, but that's not the really interesting part here; what if our pipeline weren't a pipeline? What if it were a graph?!

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

We may be reaching the limits of ASCII 'art' here but bear with me...

The implications are huge! We can remove the shackles of a the rigid fixed-order pipeline. Instead we can have application code determine the stages a request is forwarded to on the fly. For example, imagine you have a request that requires a bunch of iterations of a specific stage in the traditional pipeline. The only option you have is to perform the iterations on a single worker basically locking that worker until all the iterations are done. This would monopolize the worker; a certain request can ask for `N` iterations worth of work while the next request may only need one.

Allowing the stages to be connected in a graph structure (including cycles) would give the application the option to load balance portions of a larger request until the entire request has been fulfilled. Any problem that could be broken down into tasks of equal size (or at least more equal size) would have the potential to handle requests in a much more fairly balanced fashion.

A graph structure for the various stages also has the potential to better organize the functionalities of worker pools. One of the drawbacks of the fixed pipeline, mentioned earlier, was that to handle heterogenious request types, you would either need workers that knew how to do more than one thing or indeed run the different types of requests on different clusters. That limitation would no longer exist in a graph structure. Based on the request type the application can forward it on to the relevant stage.

For example say you wanted to offer up math as a service (MaS of course). You might have:

* workers to compute derivatives
* workers to do summations
* workers to compute integrals

Now of course you could implement this all in a client side library, but for the sake of argument, ignore the impracticality for a second. What you wouldn't want to do is write a worker does all three things. This requires forwarding to a specific worker pool based on the url (in this example). Which brings up another `TODO`, we probably want to allow the server to forward requests to worker pools based on the URL. Furthermore some of these operations are more complex than others. If you watched your system for a while (and you had high enough traffic) you could find the places where you spent more or less time and reallocate proportionally sized worker pools. You could even dynamically size the worker pools if you were really slick ;o)

The Conclusion
--------------

This has been a fantastic little experiment to have worked on. Even better its been a success. I can claim that because it's used in at least one production system. Taking some excellent tools (ZMQ mostly) and building a new tool to help others build yet more tools is a very rewarding experience. If you think you may be interested in building a project/service/tool using this work, let us know! If you find something wrong submit an issue or better yet pull request a fix!





















