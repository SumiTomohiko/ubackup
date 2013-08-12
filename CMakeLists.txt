
project(ubackup)
cmake_minimum_required(VERSION 2.8)
set(CMAKE_VERBOSE_MAKEFILE 1)

set(UBACKUP_VERSION \"1.0.0dev1\")

set(INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)
include_directories(${INCLUDE_DIR})
configure_file(
    ${INCLUDE_DIR}/ubackup/config.h.in ${INCLUDE_DIR}/ubackup/config.h)

subdirs(share/ubackup src tests)

# vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
