cmake_minimum_required(VERSION 3.10)
project(HFTMatchingEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(Threads REQUIRED)
include_directories(include)

add_executable(orderbook_matcher
    src/main.cpp
    src/OrderBook.cpp
)
target_link_libraries(orderbook_matcher PRIVATE Threads::Threads)

add_executable(orderbook_tests
    tests/test_orderbook.cpp
    src/OrderBook.cpp
)
target_link_libraries(orderbook_tests PRIVATE Threads::Threads)
