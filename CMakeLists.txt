
# CMake minimum version
cmake_minimum_required(VERSION 3.0)

# project name and languages
project(c1mockup CXX RC)

# c1mockup.exe
add_executable(c1mockup WIN32 c1mockup.cpp c1mockup_res.rc)
target_compile_definitions(c1mockup PRIVATE -DMOCKUP=1)
target_link_libraries(c1mockup comctl32 imm32)
