
add_executable(ubackupee ubackupee.c)
add_executable(ubackuper ubackuper.c)

set(CMAKE_C_COMPILER clang)
set(CMAKE_C_FLAGS "-g -Wall -Wextra -Werror -O3")

install(PROGRAMS ubackupee ubackuper DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
