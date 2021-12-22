cmake_minimum_required(VERSION 3.16)
project(vnf)

set(CMAKE_CXX_STANDARD 11)

# avoid Apple Silicon impact
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")

# import dpdk library
find_package(PkgConfig REQUIRED)
pkg_search_module(LIBDPDK REQUIRED libdpdk)

# import thread
find_package(Threads REQUIRED)

link_directories(${LIBDPDK_LIBRARY_DIRS})

include_directories(${LIBDPDK_INCLUDE_DIRS})

aux_source_directory(./ SRC_MAIN)

add_executable(vnf ${SRC_MAIN})

target_link_libraries(vnf ${LIBDPDK_LIBRARIES} Threads::Threads)