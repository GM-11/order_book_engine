find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(GEN_DIR    ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

set(COMMANDS_PROTO  ${PROTO_ROOT}/exchange/v1/commands.proto)
set(SEQUENCER_PROTO ${PROTO_ROOT}/exchange/v1/sequencer.proto)

add_library(exchange_proto STATIC)

protobuf_generate(TARGET exchange_proto
  PROTOS ${COMMANDS_PROTO} ${SEQUENCER_PROTO}
  IMPORT_DIRS ${PROTO_ROOT}
  PROTOC_OUT_DIR ${GEN_DIR})

protobuf_generate(TARGET exchange_proto
  PROTOS ${SEQUENCER_PROTO}
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
  IMPORT_DIRS ${PROTO_ROOT}
  PROTOC_OUT_DIR ${GEN_DIR})

# SYSTEM: warnings in generated headers stay out of our own build.
target_include_directories(exchange_proto SYSTEM PUBLIC ${GEN_DIR})
target_link_libraries(exchange_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
target_compile_features(exchange_proto PUBLIC cxx_std_20)
