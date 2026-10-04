find_package(Threads REQUIRED)

add_library(server_core STATIC src/worker.cpp src/router.cpp src/codec.cpp
            src/sequencer_feed.cpp src/publisher.cpp
            src/feed_codec.cpp src/engine_feed_server.cpp)
target_include_directories(server_core PUBLIC include)
target_link_libraries(server_core PUBLIC engine Threads::Threads PRIVATE
                      exchange_proto)
target_compile_features(server_core PUBLIC cxx_std_20)
target_compile_options(server_core PRIVATE -Wall -Wextra -Wpedantic)

option(SERVER_BUILD_TESTS "Build server unit tests" ${PROJECT_IS_TOP_LEVEL})
if(SERVER_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()

add_library(cli_common STATIC src/cli/console.cpp)
target_include_directories(cli_common PUBLIC src)
target_link_libraries(cli_common PUBLIC server_core)
target_compile_options(cli_common PRIVATE -Wall -Wextra -Wpedantic)

# The runnable server. For now its "gateway" is the keyboard.
add_executable(exchange_server src/main.cpp)
target_link_libraries(exchange_server PRIVATE cli_common instrument_config)
target_compile_options(exchange_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(engine_node src/engine_node.cpp)
target_link_libraries(engine_node PRIVATE cli_common instrument_config)
target_compile_options(engine_node PRIVATE -Wall -Wextra -Wpedantic)
