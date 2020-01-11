cmake_minimum_required(VERSION 2.8)
project(t265_reset)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH};${CMAKE_SOURCE_DIR}/cmake")

find_package ( LibUSB REQUIRED )

include_directories(.)

add_executable(t265_reset
        log.h
        main.cpp)

target_include_directories(t265_reset PUBLIC SYSTEM ${LIBUSB1_INCLUDE_DIR})
target_link_libraries(t265_reset ${LIBUSB1_LIBRARY})