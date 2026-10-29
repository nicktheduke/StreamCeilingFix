cmake_minimum_required(VERSION 3.16)
project(StreamCeilingFix VERSION 1.0.1 LANGUAGES C)

# Builds StreamCeilingFix.dll with MSVC (Visual Studio 2019+) or MinGW-w64.
#   cmake -B build -A x64 && cmake --build build --config Release
add_library(StreamCeilingFix SHARED src/StreamCeilingFix.c)
target_link_libraries(StreamCeilingFix PRIVATE shell32)
set_target_properties(StreamCeilingFix PROPERTIES PREFIX "" OUTPUT_NAME "StreamCeilingFix")

if(MSVC)
  target_compile_options(StreamCeilingFix PRIVATE /W4 /O2)
  target_compile_definitions(StreamCeilingFix PRIVATE _CRT_SECURE_NO_WARNINGS)
  # The plugin's own code must not depend on a separate CRT DLL being installed.
  set_property(TARGET StreamCeilingFix PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  target_compile_options(StreamCeilingFix PRIVATE -O2 -Wall -Wextra)
  target_link_options(StreamCeilingFix PRIVATE -s -static-libgcc)
endif()
