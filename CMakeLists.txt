cmake_minimum_required(VERSION 3.20)
project(MyAsioSplitter VERSION 1.0.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "MyAsioSplitter requires MSVC / MSBuild.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MyAsioSplitter is x64 only. Configure with -A x64.")
endif()

add_library(MyAsioSplitter SHARED
    src/dllmain.cpp
    src/config.cpp
    src/config.h
    src/shm_ring.h
    src/asio_abi.h
    MyAsioSplitter.def
)

target_compile_features(MyAsioSplitter PRIVATE cxx_std_20)

target_compile_definitions(MyAsioSplitter PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_compile_options(MyAsioSplitter PRIVATE /W4 /permissive- /utf-8)

target_link_libraries(MyAsioSplitter PRIVATE
    ole32
    uuid
    advapi32
    user32
    avrt
)

set_target_properties(MyAsioSplitter PROPERTIES
    OUTPUT_NAME "MyAsioSplitter"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_executable(MyAsioSplitterSmokeTest
    tests/smoke.cpp
    src/asio_abi.h
    src/shm_ring.h
)
target_compile_features(MyAsioSplitterSmokeTest PRIVATE cxx_std_20)
target_compile_definitions(MyAsioSplitterSmokeTest PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)
target_compile_options(MyAsioSplitterSmokeTest PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(MyAsioSplitterSmokeTest PRIVATE ole32)

add_executable(MyAsioSplitterShmDump
    tests/shm_dump.cpp
    src/shm_ring.h
)
target_compile_features(MyAsioSplitterShmDump PRIVATE cxx_std_20)
target_compile_definitions(MyAsioSplitterShmDump PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)
target_compile_options(MyAsioSplitterShmDump PRIVATE /W4 /permissive- /utf-8)
