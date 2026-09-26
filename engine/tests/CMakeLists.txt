include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(engine_tests
    test_matching.cpp
    test_self_trade.cpp
    test_stops.cpp
    test_circuit_breaker.cpp
    test_cancel_and_queries.cpp
    test_modify.cpp
    test_events.cpp
    test_order_ids.cpp
    test_pool.cpp
)
target_link_libraries(engine_tests PRIVATE engine Catch2::Catch2WithMain)
target_compile_features(engine_tests PRIVATE cxx_std_20)

include(Catch)
catch_discover_tests(engine_tests)
