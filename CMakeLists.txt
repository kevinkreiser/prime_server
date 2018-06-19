cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(prime_server LANGUAGES CXX C)
INCLUDE(FindPkgConfig)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use a C++11 enabled compiler
set(CMAKE_CXX_STANDARD 11)

# Handle the dependencies
pkg_check_modules(CURL REQUIRED libcurl>=7.22.0)
include_directories(${CURL_INCLUDE_DIR})

pkg_check_modules(ZMQ REQUIRED libzmq>=4.1.4)
include_directories(${ZMQ_INCLUDE_DIR})

pkg_check_modules(CZMQ REQUIRED libczmq>=3.0)
include_directories(${CZMQ_INCLUDE_DIR})

find_package (Threads REQUIRED)

# Build types
if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "No build type specified, defaulting to Release")
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_BUILD_TYPE MATCHES Debug)
  message(STATUS "Configuring in debug mode")
elseif(CMAKE_BUILD_TYPE MATCHES Release)
  message(STATUS "Configuring in release mode")
elseif(CMAKE_BUILD_TYPE MATCHES RelWithDebInfo)
  message(STATUS "Configuring in release mode with debug flags")
elseif(CMAKE_BUILD_TYPE MATCHES MinRelSize)
  message(STATUS "Configuring in release mode with minimized size")
else()
  message(STATUS "Unrecognized build type - will use cmake defaults")
endif()

# Some optional stuff
option(ENABLE_CCACHE "Speed up incremental rebuilds via ccache" ON)
option(ENABLE_COVERAGE "Build with coverage instrumentalisation" OFF)
option(ENABLE_SANITIZER "Use memory sanitizer for Debug build" OFF)

if(ENABLE_SANITIZER)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
endif()

if(ENABLE_CCACHE AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU"))
  find_program(CCACHE_FOUND ccache)
  if(CCACHE_FOUND)
    message(STATUS "Using ccache to speed up incremental builds")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
    set(ENV{CCACHE_CPP2} "true")
  endif()
endif()

if(ENABLE_COVERAGE)
  find_program(LCOV_PATH NAMES lcov lcov.bat lcov.exe lcov.perl)
  find_program(GENHTML_PATH NAMES genhtml genhtml.perl genhtml.bat)
  if(NOT LCOV_PATH OR NOT GENHTML_PATH)
    message(FATAL_ERROR "no lcov or genhtml installed")
  endif()

  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/coverage.info
    COMMAND ${LCOV_PATH} --directory . --base-directory ${CMAKE_CURRENT_SOURCE_DIR} --no-external --capture --output-file coverage.info --no-checksum
    COMMAND ${LCOV_PATH} --remove coverage.info '*/third_party/*' --output-file coverage.info
    COMMAND ${LCOV_PATH} --remove coverage.info '${CMAKE_CURRENT_BINARY_DIR}/*' --output-file coverage.info
    COMMAND ${LCOV_PATH} --list coverage.info
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS check)

  add_custom_target(coverage
    COMMAND ${GENHTML_PATH} --prefix ${CMAKE_CURRENT_BINARY_DIR} --output-directory coverage --title "Test Coverage" --legend --show-details coverage.info
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/coverage.info)
endif()

# Include the hpp files
include_directories(${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/prime_server)

set(PRIME_LIBRARY_HEADERS
	${CMAKE_SOURCE_DIR}/prime_server/prime_server.hpp
	${CMAKE_SOURCE_DIR}/prime_server/http_util.hpp
	${CMAKE_SOURCE_DIR}/prime_server/netstring_protocol.hpp
	${CMAKE_SOURCE_DIR}/prime_server/logging.hpp
	${CMAKE_SOURCE_DIR}/prime_server/zmq_helpers.hpp
	${CMAKE_SOURCE_DIR}/prime_server/http_protocol.hpp)

set(PRIME_LIBRARY_SOURCES
	${CMAKE_SOURCE_DIR}/src/http_protocol.cpp
	${CMAKE_SOURCE_DIR}/src/http_util.cpp
	${CMAKE_SOURCE_DIR}/src/netstring_protocol.cpp
	${CMAKE_SOURCE_DIR}/src/prime_server.cpp
	${CMAKE_SOURCE_DIR}/src/zmq_helpers.cpp)

# Build the library
add_library(prime_server SHARED ${PRIME_LIBRARY_SOURCES})
target_link_libraries(prime_server
	${ZMQ_LIBRARIES}
	${CZMQ_LIBRARIES}
	${CURL_LIBRARIES})

# Build executables
add_executable(prime_echod ${CMAKE_SOURCE_DIR}/src/prime_echod.cpp)
target_link_libraries(prime_echod prime_server ${CMAKE_THREAD_LIBS_INIT})

add_executable(prime_filed ${CMAKE_SOURCE_DIR}/src/prime_filed.cpp)
target_link_libraries(prime_filed prime_server ${CMAKE_THREAD_LIBS_INIT})

add_executable(prime_httpd ${CMAKE_SOURCE_DIR}/src/prime_httpd.cpp)
target_link_libraries(prime_httpd prime_server ${CMAKE_THREAD_LIBS_INIT})

add_executable(prime_serverd ${CMAKE_SOURCE_DIR}/src/prime_serverd.cpp)
target_link_libraries(prime_serverd prime_server ${CMAKE_THREAD_LIBS_INIT})

add_executable(prime_proxyd ${CMAKE_SOURCE_DIR}/src/prime_proxyd.cpp)
target_link_libraries(prime_proxyd prime_server ${CMAKE_THREAD_LIBS_INIT})

add_executable(prime_workerd ${CMAKE_SOURCE_DIR}/src/prime_workerd.cpp)
target_link_libraries(prime_workerd prime_server ${CMAKE_THREAD_LIBS_INIT})

# Install
set_target_properties(prime_server PROPERTIES PUBLIC_HEADER "${PRIME_LIBRARY_HEADERS}")

install(TARGETS prime_server
	LIBRARY DESTINATION /usr/local/lib
	PUBLIC_HEADER DESTINATION /usr/local/include)

install(TARGETS prime_echod
	prime_filed
	prime_httpd
	prime_serverd
	prime_proxyd
	prime_workerd DESTINATION /usr/local/bin)

# Add tests - Requires CTest
enable_testing()

add_executable(http ${CMAKE_SOURCE_DIR}/test/http.cpp)
target_link_libraries(http prime_server ${CMAKE_THREAD_LIBS_INIT})
add_test(http http)

add_executable(interrupt ${CMAKE_SOURCE_DIR}/test/interrupt.cpp)
target_link_libraries(interrupt prime_server ${CMAKE_THREAD_LIBS_INIT})
add_test(interrupt interrupt)

add_executable(netstring ${CMAKE_SOURCE_DIR}/test/netstring.cpp)
target_link_libraries(netstring prime_server ${CMAKE_THREAD_LIBS_INIT})
add_test(netstring netstring)

add_executable(shaping ${CMAKE_SOURCE_DIR}/test/shaping.cpp)
target_link_libraries(shaping prime_server ${CMAKE_THREAD_LIBS_INIT})
add_test(shaping shaping)

add_executable(zmq ${CMAKE_SOURCE_DIR}/test/zmq.cpp)
target_link_libraries(zmq prime_server ${CMAKE_THREAD_LIBS_INIT})
add_test(zmq zmq)
