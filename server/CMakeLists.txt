
find_package(Threads REQUIRED)

add_library(server_core INTERFACE)
target_include_directories(server_core INTERFACE include)
target_link_libraries(server_core INTERFACE engine Threads::Threads)
target_compile_features(server_core INTERFACE cxx_std_20)

option(SERVER_BUILD_TESTS "Build server unit tests" ${PROJECT_IS_TOP_LEVEL})
if(SERVER_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()
