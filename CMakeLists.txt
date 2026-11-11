cmake_minimum_required(VERSION 3.20)
project(brpc_order_matching_ecosystem CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Protobuf
find_package(Protobuf REQUIRED)

# Include Protobuf headers
include_directories(${PROTOBUF_INCLUDE_DIRS})

# Include local sources and build directories
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/proto)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Compile Protobuf definitions
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/service.proto)

# Declare targets for our ecosystem
add_executable(brpc-server
    src/server.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

add_executable(brpc-client
    src/client.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

# Find required libraries
find_package(Threads REQUIRED)
find_library(GFLAGS_LIB NAMES gflags REQUIRED)
find_library(GLOG_LIB NAMES glog REQUIRED)
find_library(LEVELDB_LIB NAMES leveldb REQUIRED)
find_library(SSL_LIB NAMES ssl REQUIRED)
find_library(CRYPTO_LIB NAMES crypto REQUIRED)
find_library(ZLIB_LIB NAMES z REQUIRED)
find_library(BRPC_LIB NAMES brpc REQUIRED)

# Link dependent libraries to bRPC Server
target_link_libraries(brpc-server PRIVATE
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES}
    ${GFLAGS_LIB}
    ${GLOG_LIB}
    ${LEVELDB_LIB}
    ${SSL_LIB}
    ${CRYPTO_LIB}
    ${ZLIB_LIB}
    Threads::Threads
    dl
)

# Link dependent libraries to bRPC Client
target_link_libraries(brpc-client PRIVATE
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES}
    ${GFLAGS_LIB}
    ${GLOG_LIB}
    ${LEVELDB_LIB}
    ${SSL_LIB}
    ${CRYPTO_LIB}
    ${ZLIB_LIB}
    Threads::Threads
    dl
)
