include(FetchContent)

# Same declaration as engine/tests; FetchContent only downloads Catch2 once.
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

# If engine/tests already fetched Catch2, the call above does nothing here,
# so Catch2's CMake helpers (Catch.cmake) are not on this directory's module
# path. Look up where Catch2 lives and add its extras/ folder explicitly.
FetchContent_GetProperties(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")

add_executable(server_tests
    test_blocking_queue.cpp
    test_commands.cpp
    test_worker.cpp
    test_router.cpp
    test_codec.cpp
    test_sequencer_feed.cpp
    test_instrument_config.cpp
    test_publisher.cpp
    test_feed_codec.cpp
    test_engine_feed.cpp
)
target_link_libraries(server_tests PRIVATE server_core exchange_proto
                      instrument_config Catch2::Catch2WithMain)
target_compile_options(server_tests PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(server_tests PRIVATE
                           ${PROJECT_SOURCE_DIR}/engine/tests)

include(Catch)

catch_discover_tests(server_tests DISCOVERY_MODE PRE_TEST)
