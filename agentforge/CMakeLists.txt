cmake_minimum_required(VERSION 3.20)
project(AgentForge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# L1 骨架：只有一个可执行目标，验证工具链。
# 后续讲次在 src/ 下增文件后，往这里补充源文件列表。
add_executable(agentforge src/main.cpp)

if (MSVC)
    target_compile_options(agentforge PRIVATE /W3 /utf-8)
    # TODO(L3+): 需要时链接 WinHTTP / Shell32 等系统库
endif()
