cmake_minimum_required(VERSION 3.16)
project(vnf)

set(CMAKE_CXX_STANDARD 11)

aux_source_directory(./ SRC_MAIN)

add_executable(vnf ${SRC_MAIN})