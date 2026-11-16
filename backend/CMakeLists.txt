cmake_minimum_required(VERSION 3.5)
project(gift_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon CONFIG REQUIRED)

add_executable(gift_server main.cpp)
target_link_libraries(gift_server PRIVATE Drogon::Drogon)