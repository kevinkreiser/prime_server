Grab some deps
--------------

    # grab the latest zmq library:
    wget http://download.zeromq.org/zeromq-4.1.0-rc1.tar.gz
    tar pxvf zeromq-4.1.0-rc1.tar.gz
    pushd zeromq-4.1.0
    ./autogen.sh
    ./configure
    make -j2
    make install
    popd

    # grab the python bindings:
    pip install pyzmq

    # grab the c++ bindings:
    wget https://raw.githubusercontent.com/zeromq/cppzmq/master/zmq.hpp -O /usr/local/include/zmq.hpp

Compile
-------

    #build:
    g++ simulator.cpp -o simulator -std=c++11 -g -O2 -lzmq
    g++ backend.cpp -o backend -std=c++11 -g -O2 -lzmq

Run it
------

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

The point
---------

Trying to simulate or run a system that is pipelined and parallelized ie. the zmq "butterfly" or "parallel pipeline" pattern. See http://zeromq.org/tutorials:butterfly. Should end up looking someting like:

    (http server)     request_producer
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
