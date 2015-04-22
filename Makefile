all:
	g++ prime_server.cpp -o prime_server -std=c++11 -g -lzmq -pthread
