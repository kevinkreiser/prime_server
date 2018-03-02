cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(prime_server LANGUAGES CXX C)

# Use a C++11 enabled compiler
set(CMAKE_CXX_STANDARD 11)

# Use the find scripts in cmake/ directory
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

# Handle the dependencies
find_package(CURL 7.22 REQUIRED)
include_directories(${CURL_INCLUDE_DIR})

find_package(ZMQ 4.1.4 REQUIRED)
include_directories(${ZMQ_INCLUDE_DIRS})

find_package(CZMQ 3.0 REQUIRED)
include_directories(${CZMQ_INCLUDE_DIRS})

# Include the hpp files
include_directories(prime_server/)

set (PRIME_LIBRARY_SOURCES
		${CMAKE_SOURCE_DIR}/src/http_protocol.cpp
		${CMAKE_SOURCE_DIR}/src/http_util.cpp
		${CMAKE_SOURCE_DIR}/src/netstring_protocol.cpp
		${CMAKE_SOURCE_DIR}/src/prime_server.cpp
		${CMAKE_SOURCE_DIR}/src/zmq_helpers.cpp)

set(PRIME_ECHOD_SOURCES
		${CMAKE_SOURCE_DIR}/src/prime_echod.cpp)

set(PRIME_FILED_SOURCES
		${CMAKE_SOURCE_DIR}/src/prime_filed.cpp)

set(PRIME_HTTPD_SOURCES
		${CMAKE_SOURCE_DIR}/src/prime_httpd.cpp)			

set(PRIME_SERVERD_SOURCES
		${CMAKE_SOURCE_DIR}/src/prime_serverd.cpp)

set(PRIME_PROXYD_SOURCES
		${CMAKE_SOURCE_DIR}/src/prime_proxyd.cpp)

set(PRIME_WORKERD_SOURCES
		${CMAKE_SOURCE_DIR}/src/prime_workerd.cpp)



# Build the library
add_library(prime_server SHARED ${PRIME_LIBRARY_SOURCES})
target_link_libraries(prime_server 
						${ZMQ_LIBRARIES} 
						${CZMQ_LIBRARIES}
						${CURL_LIBRARIES})

# Build executables
add_executable(prime_echod ${PRIME_ECHOD_SOURCES})
target_link_libraries(prime_echod prime_server pthread)

add_executable(prime_filed ${PRIME_FILED_SOURCES})
target_link_libraries(prime_filed prime_server pthread)

add_executable(prime_httpd ${PRIME_HTTPD_SOURCES})
target_link_libraries(prime_httpd prime_server pthread)

add_executable(prime_serverd ${PRIME_SERVERD_SOURCES})
target_link_libraries(prime_serverd prime_server pthread)

add_executable(prime_proxyd ${PRIME_PROXYD_SOURCES})
target_link_libraries(prime_proxyd prime_server pthread)

add_executable(prime_workerd ${PRIME_WORKERD_SOURCES})
target_link_libraries(prime_workerd prime_server pthread)



# Install
install(TARGETS prime_server LIBRARY DESTINATION lib)
install(TARGETS prime_echod
				prime_filed
				prime_httpd
				prime_serverd
				prime_proxyd
				prime_workerd DESTINATION bin)



# Add tests - Requires CTest
enable_testing()

add_executable(http ${CMAKE_SOURCE_DIR}/test/zmq.cpp)
target_link_libraries(http prime_server pthread)
add_test(http http)


add_executable(interrupt ${CMAKE_SOURCE_DIR}/test/interrupt.cpp)
target_link_libraries(interrupt prime_server pthread)
add_test(interrupt interrupt)


add_executable(netstring ${CMAKE_SOURCE_DIR}/test/netstring.cpp)
target_link_libraries(netstring prime_server pthread)
add_test(netstring netstring)

add_executable(shaping ${CMAKE_SOURCE_DIR}/test/shaping.cpp)
target_link_libraries(shaping prime_server pthread)
add_test(shaping shaping)

add_executable(zmq ${CMAKE_SOURCE_DIR}/test/zmq.cpp)
target_link_libraries(zmq prime_server pthread)
add_test(zmq zmq)
