all:
	g++ simulator.cpp -o simulator -std=c++11 -g -O2 -lzmq -pthread
#	g++ backend.cpp -o backend -std=c++11 -g -O2 -lzmq
