cmake_minimum_required(VERSION 3.20)
project(OrderBookEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
add_subdirectory(engine)

set(EXCHANGE_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto" CACHE PATH
    "Path to the shared proto folder")
if(NOT TARGET exchange_proto)
  add_subdirectory(${EXCHANGE_PROTO_DIR} ${CMAKE_BINARY_DIR}/proto)
endif()

# Shared settings (the instrument list), read by every service at startup.
set(EXCHANGE_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../config" CACHE PATH
    "Path to the shared config folder")
if(NOT TARGET instrument_config)
  add_subdirectory(${EXCHANGE_CONFIG_DIR} ${CMAKE_BINARY_DIR}/config)
endif()

add_subdirectory(server)
