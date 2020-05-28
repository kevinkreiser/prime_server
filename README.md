
             o
    .,-. .--..  .--.--. .-.     .--. .-. .--..    ._.-. .--.
    |   )|   |  |  |  |(.-'     `--.(.-' |    \  / (.-' |
    |`-' ' -' `-'  '  `-`--'____`--' `--''     `'   `--''
    |
    '

# Build Status

[![Build Status](https://travis-ci.org/kevinkreiser/prime_server.svg?branch=master)](https://travis-ci.org/kevinkreiser/prime_server)

# Quick Start

## Grab some deps

On Linux:

```bash
# trusty didn't have czmq or newer zmq in the repositories so its repackaged here
if [[ $(grep -cF trusty /etc/lsb-release) > 0 ]]; then
  sudo add-apt-repository -y ppa:kevinkreiser/libsodium
  sudo add-apt-repository -y ppa:kevinkreiser/libpgm
  sudo add-apt-repository -y ppa:kevinkreiser/zeromq3
  sudo add-apt-repository -y ppa:kevinkreiser/czmq
  sudo apt-get update
fi
# grab some standard autotools stuff
sudo apt-get install autoconf automake libtool make gcc g++ lcov
# grab curl (for url de/encode) and zmq for the awesomeness
sudo apt-get install libcurl4-openssl-dev libzmq3-dev libczmq-dev
```

On MacOS, we assume that you've installed XCode developer tools and
[Homebrew](https://brew.sh/):

```
brew install autoconf automake zmq czmq
```

## Build and Install

```bash
# dont forget submodules
git submodule update --init --recursive
# standard autotools:
./autogen.sh
./configure
make test -j8
sudo make install
```

## Run it

The library comes with a standalone binary which is essentially just a server or a simulated one that tells you whether or not a given input number is prime. The aim isn't really to do any type of novel large prime computation but rather to contrive a system whose units of work are highly non-uniform in terms of their time to completion (and yes random sleeps are boring). This is a common problem in many other workflows and primes seemed like a simple way to illustrate this.

```bash
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
```

# Motivation, Documentation and Experimentation

## The Introduction

`prime_server` is an API for building HTTP SOAs. Less the acronyms: it’s a library and executables which marry an http server to talk to clients, with a distributed computing backend to do the work. As an example we’ve made a sample application that will tell you if a given number is prime. If you’d like to give that a shot try installing and running it:

```bash
sudo add-apt-repository ppa:kevinkreiser/prime-server
sudo apt-get update
sudo apt-get install libprime-server0 libprime-server-dev prime-server-bin
prime_serverd tcp://*:8002 &
curl "http://localhost:8002/is_prime?possible_prime=32416190071"
killall prime_serverd
```

## The Point

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
                    \ _____________________ /___________________________ /


A client (a browser or just a separate thread) makes a request to a server. The server listens for new requests and replies when the backend parts send back results. The backend is comprised of load balancing proxies between layers of worker pools. In real life you may run these in different processes or on different machines. We use threads in the example server to conveniently simulate this within a single process, so please note the lack of any mutex/locking patterns (thank you ZMQ!).

This system lets you handle types of requests that can be decomposed into a single pipeline of multiple steps, which is useful if certain steps take longer than others because you can scale them independently of one another. It doesn't really handle types of requests which would need multiple pipelines, or branching pipelines, unless workers can do more than one job. To fix this we could upgrade the workers to be able to forward work to more than one proxy. This would allow heterogeneous workflows without having to make smarter/larger pluripotent workers and therefore would allow scaling of various workflows independently of each other. This sounds great but the configuration would be a mess. An easier approach would be just running a separate cluster per workflow, pros and cons there too.

You may be asking yourself, why on earth are all of the worker pools hooked into the loopback? This has two important functions:

1. A request could enter an error state at any stage of the pipeline. It's important to be able to signal this back to the client as soon as possible.
2. More generally, certain requests have known results (error or otherwise) without going through all stages of the pipeline.

## The Impetus

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

But its worse than that! You might imagine that finding a path through a graph sounds pretty straight forward but making that path useful to the client requires a bunch of extra work. Conveniently, or sometimes through great effort, decomposing a problem into discrete steps can help you with that wish list. Doing so in the context of an HTTP server API requires a little extra consideration. Basically, catering to (or hoping for) many simultaneous users makes most of those buzz-words relavant. Especially the last one (again just kidding).

## The Path

This was so fun to build for so many reasons. The first thing to do was prove out the ZMQ butterfly pattern as a tiny Github gist. The idea was that we could put an HTTP server in front of the this pattern and hit some of those pesky wishlist items just by having separate stages of the pipeline. Surprisingly, learning this pattern and figuring out how it would work in a concrete scenario was another delight.

**Public Service Anouncement** Have you read the ZeroMQ [documentation](http://zguide.zeromq.org/page:all)? I am not usually a fan of technical writing, but it is an absolute joy to read. **Warning:** it may cause you to re-think many many past code design decisions. Don't worry though, that feeling of embarassment over past poor decisions is just an indicator of making better informed ones in the future! Massive kudos to Pieter Hintjens who wrote that amazing document. If you are hungry for more good writing checkout his [blog](http://hintjens.com/) or that of another ZMQ author [Martin Sústrik](http://250bpm.com/). Both are fantastic reading!

Back to the gist. With so little setup around writing, running and debugging your code you can throw it away more easily if it doesn't work out. So we did that. Again and again, until we were left with what we came for; a parallelized pipeline whose stages had a loopback to a common entrypoint.

Next we scoured the internet for HTTP servers that had ZMQ bindings to put in front of our pipeline. As is no surprise, there are lots of good options out there! We spent some time prototyping using a few different ones but then we stumbled upon a very interesting search result. It was a [blog post](http://hintjens.com/blog:42) from Pieter Hintjens about the `ZMQ_STREAM` socket type. It describes, with examples, how the socket works and ends up making a small web server using it. Hintjens goes on to say that a lot more work would be needed for a full fledged HTTP server and describes some of the missing parts in a bit more detail.

The idea was enticing; could we build a minimal HTTP server with just ZMQ to sit in front of our pipeline? We threw some stuff into a gist once again and started testing. Before long and with very little code, we had something! From there though it was on to writing an HTTP state machine to handle the streaming nature of the socket type. Writing state machines, especially against a couple of protocol versions at the same time is torture in terms code re-use. But we'll get to that in future work section.

## The API

The API consists of essentially 3 parts:

* Client/server stuff - the bits that make and answer requests. The server stands between clients and the pipeline of workers and load balancing proxies.
* Proxy/worker stuff - the bits that fulfill the requests. The proxy sits between a pool of workers at a given stage in the pipeline and the next stage. The proxy knows what workers are available to do work and will not send on a request until a worker is available to take it. The workers are responsible to either send their results to another stage of the pipeline (the next proxy) or a sensible, protocol specific, response back to the server who will forward it on to the client.
* Protocol stuff - the bits that parse and serialize requests and responses respectively. It's a prearranged format that makes it possible for the client to speak something that the worker understands and vice-versa. HTTP is pretty useful here but other protocols exist. You can even create your own if you like. Also note that intermediate stages of workers can talk whatever protocol they like to each other.

So you want to make a web service. How can you do that… For the sake of example, let's say you want to do that all in the same process… In real life (i.e. production) you don't want to do that because, well, the wishlist again. But yeah let's just learn the easy way shall we?

Well what do you want to build? "Let's make the '**B**eautiful **U**nicode **T**ext **T**ransmission' service, the only API featuring artisanal text art!", you say. We're not sure we like where you're going with this but hey, it's your service! Let's get the library installed:

```bash
sudo add-apt-repository ppa:kevinkreiser/prime-server
sudo apt-get update
sudo apt-get install libprime-server-dev
```

Ok great, let's write our program against it, call it `art.cpp`. We'll start by including a few things we'll need:

```c++
//prime_server guts
#include <prime_server/prime_server.hpp>
#include <prime_server/http_protocol.hpp>
using namespace prime_server;

//nuts and bolts required
#include <thread>
#include <functional>
#include <chrono>
#include <string>
#include <list>
#include <vector>
#include <csignal>

//configuration constants for various sockets
const std::string server_endpoint = "tcp://*:8002";
const std::string result_endpoint = "ipc:///tmp/result_endpoint";
const std::string proxy_endpoint = "ipc:///tmp/proxy_endpoint";

//assortment of artisional content
const std::vector<std::string> art = { "(_,_)", "(_|_)", "(_*_)",
                                       "(‿ˠ‿)", "(‿ꜟ‿)", "(‿ε‿)" };
```

So first off we're including a couple of bits from `libprime_server` itself mainly for the setup of the pipeline and, as you guessed it, the stuff we need to talk the HTTP protocol. After that we include a bunch of standard data structures that we'll make use of throughout the program, it'll be pretty obvious.. Then we have some configuration. Basically we have to tell the different threads' sockets where to find each other. The first one there is a `tcp` socket so that webclients can connect to us through normal means. The other two are unix domain sockets but could be `tcp` if you want to run different parts of this on different machines. For example you could run one stage of the pipeline on machines with fat graphics cards for the GPGPU win, whereas another stage might run better on machines with metric tons of RAM. Finally we have our super sweet text art. Alright now how do we actually return a response to someone looking for some '**T**extual **U**nicode **S**tuff of **H**igh **I**ntellectual **E**xcitement'?

```c++
//actually serve up content
worker_t::result_t art_work(const std::list<zmq::message_t>& job, void* request_info) {
  //false means this is going back to the client, there is no next stage of the pipeline
  worker_t::result_t result{false};
  //this type differs per protocol hence the void* fun
  auto& info = *static_cast<http_request_t::info_t*>(request_info);
  http_response_t response;
  try {
    //TODO: actually use/validate the request parameters
    auto request = http_request_t::from_string(
      static_cast<const char*>(job.front().data()), job.front().size());
    //get your art here
    response = http_response_t(200, "OK", art[info.id % art.size()]);
  }
  catch(const std::exception& e) {
    //complain
    response = http_response_t(400, "Bad Request", e.what());
  }
  //does some tricky stuff with headers and different versions of http
  response.from_info(info);
  //formats the response to protocal that the client will understand
  result.messages.emplace_back(response.to_string());
  return result;
}
```

We basically just have to define a `work` function/object/lambda whose signature matches what the API expects. The `worker_t::result_t` is the bit that `prime_server` will be shuttling around your architecture. The bulk of this function is just stuff you have to do at every stage in your pipeline. You'll need to unpack the message from the previous stage; in this case it was the server itself so the `work` function assumes its valid HTTP-looking bytes. You'll then want to formulate a response, either to be sent back to the client or forwarded to the next stage in the pipeline. In our case the workers respond to the client in all scenarios so we always initialize `worker_t::result_t::intermediate` as `false`. If it were `true` the worker would attempt to forward the result of this stage to the proxy for the next pool of workers. At the end we simply do some formatting to the response so that the client will make sense of it and we store this in `worker_t::result_t::messages`. OK so now what is left to do? Not much, just hook up some plumbing, basically constructing your pipeline.

```c++
int main(void) {
  zmq::context_t context;

  //http server, false turns off request/response logging
  std::thread server = std::thread(std::bind(&http_server_t::serve,
    http_server_t(context, server_endpoint, proxy_endpoint + "_upstream", result_endpoint, false)));

  //load balancer
  std::thread proxy(
    std::bind(&proxy_t::forward,
      proxy_t(context, proxy_endpoint + "_upstream", proxy_endpoint + "_downstream")));
  proxy.detach();

  //workers
  auto worker_concurrency = std::max<size_t>(1, std::thread::hardware_concurrency());
  std::list<std::thread> workers;
  for(size_t i = 0; i < worker_concurrency; ++i) {
    //worker function could be defined inline here via lambda, it could be std::bind'd to an instance method
    //or simply just a free function like we have here
    workers.emplace_back(std::bind(&worker_t::work,
      worker_t(context, proxy_endpoint + "_downstream", "ipc:///tmp/NO_ENDPOINT", result_endpoint, &art_work)));
    workers.back().detach();
  }

  //listen for SIGINT and terminate if we hear it
  std::signal(SIGINT, [](int s){ std::this_thread::sleep_for(std::chrono::seconds(1)); exit(1); });
  server.join();

  return 0;
}
```

First things first, all `zmq` communication requires a `context`. So we get one of those and pass it around to all the bits. Our setup is really simple, we run an `http_server_t` in one thread. The server keeps track of and forwards requests on to a load balancing `proxy_t` in another thread. The proxy keeps a queue of requests and shuttles them FIFO style to the first non-busy `worker_t` that it has in its inventory. We spawn a bunch of `worker_t`s which are constantly handshaking with the proxy. "I'm here" and "I'm done" messages let the proxy know which workers are bored and which are busy. This should minimize latency in so far as a greedy scheduler can. You'll notice the program is pretty much meant to be run as a daemon which is why it waits for `SIGINT`. `ctl-c` it away when you are done with it.

Now we'll get your **R**esponsive **U**nicode **M**essages **P**ortal shaking.. err.. cracking.. err.. running.. with this:

```bash
g++ art.cpp -std=c++11 -lprime_server -o art
./art
```

From another terminal, you can hit it with curl to bask in your glorious, yet somewhat inappropriate, art work:

```bash
k@k:~$ for i in {0..7}; do curl "localhost:8002"; echo; done
(_,_)
(_|_)
(_*_)
(‿ˠ‿)
(‿ꜟ‿)
(‿ε‿)
(_,_)
(_|_)
```

If you're interested in more sample code you can check out [Valhalla](http://github.com/valhalla/valhalla) or any of the sample daemon programs (in `src/*d.cpp`) in the [prime_server source](https://github.com/kevinkreiser/prime_server/tree/master/src).

## The Future

The first thing we should do is make use of a proper HTTP parser. There are some impressive ones out there, notibly [PicoHTTPParser](https://github.com/h2o/picohttpparser) which is used in one of the webservers ([H2O](https://github.com/h2o/h2o)) we came across in our searching. There may be a few issues with the streaming nature of the `ZMQ_STREAM` socket but they are worth working out so as not to have to maintain the mess of code required to properly parse HTTP.

The second thing we want to do is work `zbeacon` perks into the API. Currently the setup of a pipeline is somewhat cumbersome. Each stage must know about the previous and next (optional) stages as well as the loopback. It's clunky and requires a decent understanding to get it right. It's also very manual. With `zbeacon`, applications can broadcast their endpoints to peers so that they can connect to eachother through discovery rather than via manual configuration.

Automatic service discovery is pretty great, but that's not the really interesting part here; what if our pipeline weren't a pipeline? What if it were a graph?!

    client <---> server _________
                 ^ | ^            \
                 | | |             v
                 | | |       =============
                 | | |   .-> |   proxy   | <--.
                 | | |  /    |-----------|     \
                 | | |  \    |  workers  |      |
                 | | |   \ _ |    ...    | _    |
                 | | |       =============   \  |
                 | |  \ _________ /           | |
                 |  \ ___________             | |
                 |                \           | |
                 |                 \          | |
                 |                  v         | |
                 |          =============    /  |
                 |      .-> |   proxy   | <-'   |
                 |     /    |-----------|       |
                 |     \    |  workers  |       |
                 |      \ _ |    ...    | ____ /
                 |          =============
                  \ ____________ /


We may be reaching the limits of ASCII 'art' here but bear with me...

The implications are huge! We can remove the shackles of a the rigid fixed-order pipeline. Instead we can have application code determine the stages a request is forwarded to on the fly. For example, imagine you have a request that requires a bunch of iterations of a specific stage in the traditional pipeline. The only option you have is to perform the iterations on a single worker basically locking that worker until all the iterations are done. This would monopolize the worker; a certain request can ask for `N` iterations worth of work while the next request may only need one.

Allowing the stages to be connected in a graph structure (including cycles) would give the application the option to load balance portions of a larger request until the entire request has been fulfilled. Any problem that could be broken down into tasks of equal size (or at least more equal size) would have the potential to handle requests in a much more fairly balanced fashion.

A graph structure for the various stages also has the potential to better organize the functionalities of worker pools. One of the drawbacks of the fixed pipeline, mentioned earlier, was that to handle heterogenious request types, you would either need workers that knew how to do more than one thing or indeed run the different types of requests on different clusters. That limitation would no longer exist in a graph structure. Based on the request type the application can just forward it to the relevant stage.

For example say you wanted to offer up math as a service (MaS of course). You might have:

* workers to compute derivatives
* workers to do summations
* workers to compute integrals

Now of course you could implement this all in a client side library, but for the sake of argument, ignore the impracticality for a second. What you wouldn't want to do is write a worker that does all three things. It would be nicer to isolate workers based on the type of work they perform (again the wishlist). This requires forwarding to a specific worker pool based on the url (in this example). Which brings up another `TODO`, we probably want to allow the server to forward requests to worker pools based on the URL (lots of other servers have this). Furthermore some of these operations are more complex than others. If you watched your system for a while (with a statistically relevant amount of traffic) you could look at the amount of CPU spent per stage and reallocate proportionally sized worker pools. You could even dynamically size the worker pools based on current traffic if you were really slick ;o)

## The Conclusion

This has been a fantastic little experiment to have worked on. Even better it's been successful. I can claim that because it's used in at least one production system. Taking some excellent tools (ZMQ mostly) and building a new tool to help others build yet more tools is a very rewarding experience. If you think you may be interested in building a project/service/tool using this work, let us know! If you find something wrong submit an issue or better yet pull request a fix!
