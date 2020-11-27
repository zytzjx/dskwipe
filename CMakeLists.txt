cmake_minimum_required(VERSION 3.0.0)
project(dskwipe VERSION 0.1.0)

include(CTest)
enable_testing()

include_directories(${CMAKE_SOURCE_DIR}/sg3_utils-1.45/include)
link_directories(${CMAKE_SOURCE_DIR}/sg3_utils-1.45/lib)

add_executable(dskwipe sg_dd.c getopt_long.c util.c)
target_link_libraries(dskwipe sgutils2)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
