# Server layer: routes commands to per-symbol worker threads that own engine Books.
find_package(Threads REQUIRED)

add_library(server_core STATIC src/worker.cpp)
target_include_directories(server_core PUBLIC include)
target_link_libraries(server_core PUBLIC engine Threads::Threads)
target_compile_features(server_core PUBLIC cxx_std_20)
target_compile_options(server_core PRIVATE -Wall -Wextra -Wpedantic)

option(SERVER_BUILD_TESTS "Build server unit tests" ${PROJECT_IS_TOP_LEVEL})
if(SERVER_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()
