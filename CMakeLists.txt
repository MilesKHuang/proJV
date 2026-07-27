cmake_minimum_required(VERSION 3.16)
project(proJV VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Paths ────────────────────────────────────────────────────────────────
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(IMGUI_DIR    "${EXTERNAL_DIR}/imgui")
set(SRC_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ── SQLiteCpp (both platforms, from source) ──────────────────────────────
add_subdirectory(
    "${EXTERNAL_DIR}/SQLiteCpp-3.3.3"
    "${CMAKE_BINARY_DIR}/SQLiteCpp"
)

# ── GLFW (both platforms, from source) ───────────────────────────────────
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXTERNAL_DIR}/glfw)

# ── Common sources (platform-agnostic business logic) ────────────────────
set(COMMON_SOURCES
    ${SRC_DIR}/gui_backend.cpp
    ${SRC_DIR}/client/models.cpp
    ${SRC_DIR}/client/deepseek.cpp
    ${SRC_DIR}/core/config.cpp
    ${SRC_DIR}/core/agent.cpp
    ${SRC_DIR}/core/session.cpp
    ${SRC_DIR}/tools/registry.cpp
    ${SRC_DIR}/tools/file_tool.cpp
    ${SRC_DIR}/tools/md_file_tool.cpp
    ${SRC_DIR}/tools/edit_file_tool.cpp
    ${SRC_DIR}/tools/file_search_tool.cpp
    ${SRC_DIR}/tools/web_tools.cpp
    ${SRC_DIR}/tools/todo_tool.cpp
    ${SRC_DIR}/core/storage.cpp
    ${SRC_DIR}/core/tool_list_parser.cpp
    ${SRC_DIR}/core/prompts_loader.cpp
    ${SRC_DIR}/ui/app.cpp
    ${SRC_DIR}/ui/render_chat.cpp
    ${SRC_DIR}/ui/render_settings.cpp
    ${SRC_DIR}/ui/markdown_render.cpp
    ${SRC_DIR}/ui/theme.cpp
    ${SRC_DIR}/ui/theme_popup.cpp
    ${EXTERNAL_DIR}/loguru.cpp
)

set(HEADERS
    ${SRC_DIR}/gui_backend.h
    ${SRC_DIR}/client/models.h
    ${SRC_DIR}/client/deepseek.h
    ${SRC_DIR}/core/config.h
    ${SRC_DIR}/core/agent.h
    ${SRC_DIR}/core/session.h
    ${SRC_DIR}/core/storage.h
    ${SRC_DIR}/tools/registry.h
    ${SRC_DIR}/tools/file_tool.h
    ${SRC_DIR}/tools/python_tool_manager.h
    ${SRC_DIR}/tools/json_utils.h
)

# ── Platform-specific sources ────────────────────────────────────────────
if(WIN32)
    set(PLATFORM_SOURCES
        ${SRC_DIR}/tools/shell_tool.cpp
        ${SRC_DIR}/tools/python_tool_manager.cpp
    )
else()
    # S4 will add process_runner_linux.cpp, system_util_linux.cpp here.
    # For now shell_tool.cpp's #else branch (popen) provides basic Linux support.
    set(PLATFORM_SOURCES
        ${SRC_DIR}/tools/shell_tool.cpp
        ${SRC_DIR}/tools/python_tool_manager.cpp
    )
endif()

# ── libcurl: Windows from source, Linux from system ──────────────────────
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows native SSL" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    add_subdirectory(${EXTERNAL_DIR}/curl-8.21.0)
    set(CURL_INCLUDE_DIR "${EXTERNAL_DIR}/curl-8.21.0/include")
    set(CURL_LIBRARY libcurl)
else()
    find_package(CURL REQUIRED)
    set(CURL_INCLUDE_DIR ${CURL_INCLUDE_DIRS})
    set(CURL_LIBRARY ${CURL_LIBRARIES})
endif()

# ── Release build: strip debug logs ──────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_definitions(PROJV_RELEASE)
endif()

# ── Static library (proJV_lib) ───────────────────────────────────────────
add_library(proJV_lib STATIC ${COMMON_SOURCES} ${PLATFORM_SOURCES} ${HEADERS})

# Common includes
target_include_directories(proJV_lib PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${EXTERNAL_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/client
    ${CURL_INCLUDE_DIR}
    ${EXTERNAL_DIR}/glfw/include
)

# ── Platform: Windows ────────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(proJV_lib PRIVATE
        opengl32
        crypt32 ole32 shell32 comctl32 comdlg32 gdi32 user32 dbghelp advapi32
        ${CURL_LIBRARY}
        SQLiteCpp
    )
    target_compile_definitions(proJV_lib PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/wd4005>")
    endif()

# ── Platform: Linux ──────────────────────────────────────────────────────
else()
    find_package(OpenGL REQUIRED)
    find_package(X11 REQUIRED)
    target_link_libraries(proJV_lib PRIVATE
        OpenGL::GL
        ${CURL_LIBRARY}
        SQLiteCpp
        pthread
        dl
        ${X11_LIBRARIES}
    )
    target_include_directories(proJV_lib PRIVATE ${X11_INCLUDE_DIR})
endif()

# ── GUI executable ───────────────────────────────────────────────────────
if(WIN32)
    add_executable(proJV WIN32 ${SRC_DIR}/main.cpp)
else()
    add_executable(proJV ${SRC_DIR}/main.cpp)
endif()

target_include_directories(proJV PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${EXTERNAL_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/client
    ${EXTERNAL_DIR}/glfw/include
)
target_link_libraries(proJV PRIVATE
    proJV_lib
    glfw
)

if(WIN32)
    target_compile_definitions(proJV PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        set_target_properties(proJV PROPERTIES
            LINK_FLAGS "/ENTRY:mainCRTStartup"
        )
    endif()
endif()

# ── imgui sources (OpenGL3 + GLFW backends, both platforms) ──────────────
target_sources(proJV PRIVATE
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(proJV PRIVATE ${IMGUI_DIR}/backends)

# ── Copy assets + projv_files to build directory ─────────────────────────
add_custom_command(TARGET proJV POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:proJV>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/projv_files"
        "$<TARGET_FILE_DIR:proJV>/projv_files"
    COMMENT "Copying assets and projv_files to build output"
)
