cmake_minimum_required(VERSION 3.20)
project(AgentForgeGUI LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC OFF)          # 本界面不使用 Q_OBJECT 信号槽，无需 moc

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(AgentForgeGUI
    gui_main.cpp
    main_window.cpp

    # ---- 核心层（与 CLI 共享）----
    ${ROOT}/src/agent_loop.cpp
    ${ROOT}/src/context_budget.cpp
    ${ROOT}/src/providers.cpp
    ${ROOT}/src/sse_parser.cpp
    ${ROOT}/src/http_winhttp.cpp
    ${ROOT}/src/tool_registry.cpp
    ${ROOT}/src/plugin.cpp
    ${ROOT}/src/sandbox_win.cpp
    ${ROOT}/src/logging.cpp
    ${ROOT}/src/repo_index.cpp
    ${ROOT}/src/config.cpp
    ${ROOT}/src/secret_win.cpp
    ${ROOT}/src/session_store.cpp
    ${ROOT}/src/tools.cpp

    # ---- 第三方（SQLite 官方 amalgamation）----
    ${ROOT}/thirdparty/sqlite/sqlite3.c
)

target_include_directories(AgentForgeGUI PRIVATE
    ${ROOT}/src
    ${ROOT}/thirdparty
    ${ROOT}/thirdparty/sqlite
)

target_compile_definitions(AgentForgeGUI PRIVATE
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION
)

if (MSVC)
    target_compile_options(AgentForgeGUI PRIVATE /utf-8 /W3)
    target_link_options(AgentForgeGUI PRIVATE "/MAP:${CMAKE_BINARY_DIR}/AgentForgeGUI.map")
endif()

target_link_libraries(AgentForgeGUI PRIVATE Qt6::Widgets winhttp crypt32 shell32)


