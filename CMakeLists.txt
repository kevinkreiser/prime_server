cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
include(cmake/version.cmake)
get_version(${CMAKE_SOURCE_DIR}/prime_server/prime_server.hpp "PRIME_SERVER_")
project(prime_server LANGUAGES CXX C VERSION ${PRIME_SERVER_VERSION_MAJOR}.${PRIME_SERVER_VERSION_MINOR}.${PRIME_SERVER_VERSION_PATCH})
INCLUDE(FindPkgConfig)

# Use a C++11 enabled compiler
set(CMAKE_CXX_STANDARD 11)

# What type of build
if(NOT MSVC_IDE) # TODO: May need to be extended for Xcode, CLion, etc.
  if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type specified, defaulting to Release")
    set(CMAKE_BUILD_TYPE Release)
  endif()
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
  set(CMAKE_BUILD_TYPE Release)
  message(STATUS "Unrecognized build type defaulting to release")
endif()

# Show all warnings and treat as errors
if(MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -pedantic -Werror)
endif()

option(ENABLE_SANITIZERS "Use all the integrated sanitizers for Debug build" OFF)
option(ENABLE_ADDRESS_SANITIZER "Use memory sanitizer for Debug build" OFF)
option(ENABLE_UNDEFINED_SANITIZER "Use UB sanitizer for Debug build" OFF)
include(cmake/sanitizer.cmake)

# Handle the dependencies
pkg_check_modules(CURL REQUIRED libcurl>=7.22.0)
include_directories(${CURL_INCLUDEDIR})

pkg_check_modules(ZMQ REQUIRED libzmq>=4.1.4)
include_directories(${ZMQ_INCLUDEDIR})

pkg_check_modules(CZMQ REQUIRED libczmq>=3.0)
include_directories(${CZMQ_INCLUDEDIR})

find_package (Threads REQUIRED)

# Include the hpp files
include_directories(${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/prime_server)

set(PRIME_LIBRARY_HEADERS
	${CMAKE_SOURCE_DIR}/prime_server/prime_server.hpp
	${CMAKE_SOURCE_DIR}/prime_server/http_util.hpp
	${CMAKE_SOURCE_DIR}/prime_server/netstring_protocol.hpp
	${CMAKE_SOURCE_DIR}/prime_server/zmq_helpers.hpp
	${CMAKE_SOURCE_DIR}/prime_server/http_protocol.hpp)

set(PRIME_LIBRARY_SOURCES
	${CMAKE_SOURCE_DIR}/src/logging/logging.hpp
	${CMAKE_SOURCE_DIR}/src/prime_helpers.hpp
	${CMAKE_SOURCE_DIR}/src/http_protocol.cpp
	${CMAKE_SOURCE_DIR}/src/http_util.cpp
	${CMAKE_SOURCE_DIR}/src/netstring_protocol.cpp
	${CMAKE_SOURCE_DIR}/src/prime_server.cpp
	${CMAKE_SOURCE_DIR}/src/zmq_helpers.cpp)

# Build the library
add_library(prime_server SHARED ${PRIME_LIBRARY_SOURCES})
target_link_libraries(prime_server
  PUBLIC
    ${ZMQ_LDFLAGS}
  PRIVATE
    ${CZMQ_LDFLAGS}
    ${CURL_LDFLAGS})

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

set_target_properties(prime_server PROPERTIES
	PUBLIC_HEADER "${PRIME_LIBRARY_HEADERS}"
	SOVERSION ${PROJECT_VERSION_MAJOR}
	VERSION ${PROJECT_VERSION})

# Install paths
set(prefix "${CMAKE_INSTALL_PREFIX}")
set(exec_prefix "${CMAKE_INSTALL_PREFIX}")
set(bindir "${CMAKE_INSTALL_PREFIX}/bin")
set(libdir "${CMAKE_INSTALL_PREFIX}/lib")
set(includedir "${CMAKE_INSTALL_PREFIX}/include/prime_server")
set(pkgconfigdir "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig")
set(VERSION ${PROJECT_VERSION})

install(TARGETS prime_server
	LIBRARY DESTINATION ${libdir}
	PUBLIC_HEADER DESTINATION ${includedir})

install(TARGETS prime_echod
	prime_filed
	prime_httpd
	prime_serverd
	prime_proxyd
	prime_workerd DESTINATION ${bindir})

CONFIGURE_FILE(
	"${CMAKE_CURRENT_SOURCE_DIR}/libprime_server.pc.in"
	"${CMAKE_CURRENT_BINARY_DIR}/libprime_server.pc"
	@ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libprime_server.pc" DESTINATION ${pkgconfigdir})

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
