# dont forget submodules

git submodule update --init --recursive
# standard autotools:
./autogen.sh
./configure
# make test -j8
sudo make install
g++ src/prime_testd.cpp -lzmq -lprime_server -std=c++11 -o prime_testd
echo Running prime_testd...
./prime_testd tcp://0.0.0.0:8080 2 20,30

