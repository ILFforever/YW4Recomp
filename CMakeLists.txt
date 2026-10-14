cmake_minimum_required(VERSION 3.24)

project(yw4recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Refuse in-source builds so generated files never land next to tracked ones.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Build out of source, e.g. cmake -B build")
endif()

# runtime/ and recomp/ get their own CMakeLists.txt once there is code to
# build (Phase 1 and Phase 2). Phase 0 work is Python under tools/.
