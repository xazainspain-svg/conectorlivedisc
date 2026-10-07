cmake_minimum_required(VERSION 3.22)
project(ldsc_device CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(ldsc-device ldsc-device.cpp engine.h)
target_compile_definitions(ldsc-device PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(ldsc-device PRIVATE ole32 avrt ws2_32 uuid)

add_executable(ldsc-tray WIN32 ldsc-tray.cpp engine.h)
target_compile_definitions(ldsc-tray PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(ldsc-tray PRIVATE ole32 avrt ws2_32 uuid user32 shell32 advapi32)
