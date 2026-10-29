# ---------------------------------------------------------------------------
#  L1 骨架的 CMake 配置
#
#  注意：本机（CMake 4.4.2 + VS 2026）CMake 在 configure 阶段认不出工具集，
#  会报 "No CMAKE_CXX_COMPILER could be found"。这是 CMake 与环境的问题，
#  与代码无关。**Windows 上请用同目录的 build.ps1。**
#
#  Linux / macOS 上用 CMake 是正常的：
#      cmake -B build && cmake --build build -j
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(agentforge_l1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(agentforge main.cpp)

if(MSVC)
  # /utf-8 必加：源文件是无 BOM 的 UTF-8，中文注释不显式指定会被按 GBK 解析，
  # 报出一堆莫名其妙的 C3872（标识符中不允许此字符）/ C1075。
  target_compile_options(agentforge PRIVATE /W3 /utf-8 /permissive-)
  target_compile_definitions(agentforge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(agentforge PRIVATE -Wall -Wextra)
endif()
