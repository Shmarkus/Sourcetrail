cmake_minimum_required(VERSION 3.16)
project(shop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# compile_commands.json for Sourcetrail: cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(shop src/item.cpp src/order.cpp src/pricing.cpp)
target_include_directories(shop PUBLIC include)

add_executable(shop_app app/main.cpp)
target_link_libraries(shop_app PRIVATE shop)

enable_testing()
add_executable(shop_tests tests/shop_tests.cpp)
target_link_libraries(shop_tests PRIVATE shop)
add_test(NAME shop_tests COMMAND shop_tests)
