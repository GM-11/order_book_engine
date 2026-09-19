add_library(engine STATIC src/order.cpp src/book.cpp)

target_include_directories(engine PUBLIC include PRIVATE src)

target_compile_features(engine PUBLIC cxx_std_20)

target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
