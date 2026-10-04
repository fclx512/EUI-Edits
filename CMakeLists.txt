cmake_minimum_required(VERSION 3.14)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(EUI-NEO VERSION 0.6.0)

# NeoEditor has its own product version; do not change the framework version.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NeoEditorVersion.cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(EUI_NEO_TOP_LEVEL_DEFAULT ON)
else()
    set(EUI_NEO_TOP_LEVEL_DEFAULT OFF)
endif()
option(EUI_BUILD_APPS "Build short bundled API examples from examples/." ${EUI_NEO_TOP_LEVEL_DEFAULT})
option(EUI_BUILD_USER_APPS "Build longer or multi-page applications from apps/." ${EUI_NEO_TOP_LEVEL_DEFAULT})
option(EUI_BUILD_NEOEDITOR_ONLY "Configure NeoEditor without other bundled applications." OFF)
if(EUI_BUILD_NEOEDITOR_ONLY)
    set(EUI_BUILD_APPS OFF)
    set(EUI_BUILD_USER_APPS ON)
    if(MSVC)
        # Keep compiler debug records relocatable. Runtime asset lookup below
        # separately disables the development source-directory fallback.
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" NEO_EDITOR_PATHMAP_SOURCE)
        add_compile_options("$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:/pathmap:${NEO_EDITOR_PATHMAP_SOURCE}=.>")
    endif()
endif()
set(EUI_USER_APP_SKIP "" CACHE STRING "User apps (apps/) to skip building, e.g. \"calculator;clock\". Leave neo_editor out of the list to keep the editor buildable.")
option(EUI_BUILD_TEST_FIXTURES "Build internal test fixture applications." OFF)
option(EUI_BUILD_FFMPEG_VIDEO_EXAMPLE "Build the optional FFmpeg video playback example." OFF)
option(EUI_BUILD_SHARED "Build eui_neo as a shared library instead of a static library." OFF)
option(EUI_ENABLE_INSTALL "Install EUI-NEO library, headers, and CMake package files." ${EUI_NEO_TOP_LEVEL_DEFAULT})
option(EUI_ENABLE_MODULES "Build optional EUI-NEO modules when their directories are present." ${EUI_NEO_TOP_LEVEL_DEFAULT})
option(EUI_VULKAN_LOW_LATENCY_PRESENT "Prefer low-latency Vulkan presentation when available." OFF)
option(EUI_ENABLE_TRAY "Enable the system tray backend. On Linux, configuration fails when neither glib/gio (SNI) nor GTK3 + libappindicator is available; turn this OFF to build without tray support." ON)

set(EUI_DEFAULT_WINDOW_BACKEND "glfw")
set(EUI_DEFAULT_RENDER_BACKEND "opengl")
get_filename_component(EUI_BUILD_DIR_NAME "${CMAKE_BINARY_DIR}" NAME)
string(TOLOWER "${EUI_BUILD_DIR_NAME}" EUI_BUILD_DIR_NAME_LOWER)
if(EUI_BUILD_DIR_NAME_LOWER MATCHES "(^|[-_])sdl2($|[-_])")
    set(EUI_DEFAULT_WINDOW_BACKEND "sdl2")
endif()
if(EUI_BUILD_DIR_NAME_LOWER MATCHES "(^|[-_])(vk|vulkan)($|[-_])")
    set(EUI_DEFAULT_RENDER_BACKEND "vulkan")
endif()

set(EUI_WINDOW_BACKEND "${EUI_DEFAULT_WINDOW_BACKEND}" CACHE STRING "Window backend: glfw, sdl2, or win32")
set_property(CACHE EUI_WINDOW_BACKEND PROPERTY STRINGS glfw sdl2 win32)
set(EUI_RENDER_BACKEND "${EUI_DEFAULT_RENDER_BACKEND}" CACHE STRING "Render backend: auto, opengl, vulkan, or d2d (Windows experiment)")
set_property(CACHE EUI_RENDER_BACKEND PROPERTY STRINGS auto opengl vulkan d2d)
if(NOT EUI_WINDOW_BACKEND MATCHES "^(glfw|sdl2|win32)$")
    message(FATAL_ERROR "EUI_WINDOW_BACKEND must be one of: glfw, sdl2, win32")
endif()
if(NOT EUI_RENDER_BACKEND MATCHES "^(auto|opengl|vulkan|d2d)$")
    message(FATAL_ERROR "EUI_RENDER_BACKEND must be one of: auto, opengl, vulkan, d2d")
endif()
if(EUI_RENDER_BACKEND STREQUAL "d2d" AND (NOT WIN32 OR EUI_WINDOW_BACKEND STREQUAL "sdl2"))
    message(FATAL_ERROR "The experimental d2d backend requires Windows and GLFW or Win32.")
endif()

if(EUI_WINDOW_BACKEND STREQUAL "win32" AND (NOT WIN32 OR NOT EUI_RENDER_BACKEND STREQUAL "d2d"))
    message(FATAL_ERROR "The Win32 experiment requires Windows and EUI_RENDER_BACKEND=d2d.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

# windres uses a shell pipe for preprocessing by default. On Windows that can
# intermittently lose quoting when the MinGW toolchain path contains spaces.
if(MINGW AND CMAKE_RC_COMPILER MATCHES "windres(\\.exe)?$")
    if(NOT CMAKE_RC_FLAGS MATCHES "(^|[ \\t])--use-temp-file([ \\t]|$)")
        string(APPEND CMAKE_RC_FLAGS " --use-temp-file")
    endif()
endif()

set(EUI_MINGW_STATIC_LINK_OPTIONS -static)
if(MINGW AND NOT EUI_BUILD_SHARED)
    include(CheckCXXSourceCompiles)
    set(EUI_MINGW_STATIC_LINK_PROBE [=[
        #include <cstdio>
        #include <thread>
        int main() {
            char buffer[8]{};
            std::snprintf(buffer, sizeof(buffer), "%s", "ok");
            std::thread worker([] {});
            worker.join();
            return buffer[0] == 'o' ? 0 : 1;
        }
    ]=])
    set(EUI_SAVED_REQUIRED_LINK_OPTIONS "${CMAKE_REQUIRED_LINK_OPTIONS}")
    set(EUI_SAVED_REQUIRED_QUIET "${CMAKE_REQUIRED_QUIET}")
    set(CMAKE_REQUIRED_QUIET TRUE)
    set(CMAKE_REQUIRED_LINK_OPTIONS -static)
    check_cxx_source_compiles("${EUI_MINGW_STATIC_LINK_PROBE}"
        EUI_MINGW_DEFAULT_STATIC_RUNTIME_LINKS)
    if(NOT EUI_MINGW_DEFAULT_STATIC_RUNTIME_LINKS)
        set(CMAKE_REQUIRED_LINK_OPTIONS -static -mcrtdll=ucrt)
        check_cxx_source_compiles("${EUI_MINGW_STATIC_LINK_PROBE}"
            EUI_MINGW_UCRT_STATIC_RUNTIME_LINKS)
        if(EUI_MINGW_UCRT_STATIC_RUNTIME_LINKS)
            list(APPEND EUI_MINGW_STATIC_LINK_OPTIONS -mcrtdll=ucrt)
            message(STATUS "MinGW static runtime requires UCRT link compatibility")
        else()
            message(FATAL_ERROR
                "The selected MinGW toolchain cannot link its static runtime. "
                "Both the default CRT and UCRT compatibility mode failed.")
        endif()
    endif()
    set(CMAKE_REQUIRED_LINK_OPTIONS "${EUI_SAVED_REQUIRED_LINK_OPTIONS}")
    set(CMAKE_REQUIRED_QUIET "${EUI_SAVED_REQUIRED_QUIET}")
endif()

if(MINGW AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
    message(FATAL_ERROR
        "EUI-NEO requires MinGW-w64 GCC 12 or newer; "
        "detected GCC ${CMAKE_CXX_COMPILER_VERSION}. "
        "Use a current MinGW-w64 distribution instead of legacy TDM-GCC.")
endif()

if(APPLE)
    enable_language(OBJC)
    set_source_files_properties(core/platform/native_bridge.c PROPERTIES LANGUAGE OBJC)
    set_source_files_properties(core/platform/tray_bridge.c PROPERTIES LANGUAGE OBJC)
    set_source_files_properties(core/platform/ime_bridge.c PROPERTIES LANGUAGE OBJC)
endif()

function(eui_apply_compile_options target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /utf-8
            /wd4530
            $<$<NOT:$<CONFIG:Debug>>:/O1>
            $<$<NOT:$<CONFIG:Debug>>:/GS->
            $<$<NOT:$<CONFIG:Debug>>:/sdl->
            $<$<NOT:$<CONFIG:Debug>>:/wd4819>
        )
    elseif(APPLE)
        target_compile_options(${target_name} PRIVATE
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-Os>
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>,$<NOT:$<STREQUAL:${target_name},vcd_viewer>>,$<NOT:$<STREQUAL:${target_name},gpu_image_probe>>>:-fno-exceptions>
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-fno-rtti>
        )
    else()
        target_compile_options(${target_name} PRIVATE
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-Os>
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>,$<NOT:$<STREQUAL:${target_name},vcd_viewer>>,$<NOT:$<STREQUAL:${target_name},gpu_image_probe>>>:-fno-exceptions>
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-fno-rtti>
        )
    endif()
endfunction()

function(eui_apply_app_link_options target_name)
    get_property(_eui_build_shared GLOBAL PROPERTY EUI_NEO_BUILD_SHARED)
    get_property(_eui_mingw_static_link_options GLOBAL PROPERTY EUI_NEO_MINGW_STATIC_LINK_OPTIONS)
    if(MSVC)
        set_target_properties(${target_name} PROPERTIES VS_GLOBAL_VcpkgXUseBuiltInApplocalDeps true)
        target_link_options(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
            $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
            $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>
            /ENTRY:mainCRTStartup
            /SUBSYSTEM:WINDOWS
        )
    elseif(MINGW)
        target_link_options(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>
            $<$<NOT:$<CONFIG:Debug>>:-s>
            -mwindows
        )
        if(NOT _eui_build_shared)
            target_link_options(${target_name} PRIVATE ${_eui_mingw_static_link_options})
        endif()
    elseif(APPLE)
        target_link_options(${target_name} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip>)
    else()
        target_link_options(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>
            $<$<NOT:$<CONFIG:Debug>>:-s>
        )
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Dependencies and backend selection
# -----------------------------------------------------------------------------

set(EUI_VULKAN_BACKEND_SOURCES
    core/render/vulkan/vulkan_backend.cpp
    core/render/vulkan/vulkan_cache.cpp
    core/render/vulkan/vulkan_primitives.cpp
    core/render/vulkan/vulkan_polygon.cpp
    core/render/vulkan/vulkan_shadertoy.cpp
    core/render/vulkan/vulkan_text.cpp
    core/render/vulkan/vulkan_image.cpp
)
if(EUI_RENDER_BACKEND STREQUAL "auto" OR EUI_RENDER_BACKEND STREQUAL "vulkan")
    find_package(Vulkan QUIET)
endif()

set(EUI_RESOLVED_RENDER_BACKEND "${EUI_RENDER_BACKEND}")
if(EUI_RENDER_BACKEND STREQUAL "auto")
    if(Vulkan_FOUND)
        set(EUI_RESOLVED_RENDER_BACKEND "vulkan")
    else()
        set(EUI_RESOLVED_RENDER_BACKEND "opengl")
        message(STATUS "Vulkan SDK not found; falling back to OpenGL.")
    endif()
elseif(EUI_RENDER_BACKEND STREQUAL "vulkan")
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR "EUI_RENDER_BACKEND=vulkan requested, but Vulkan SDK was not found.")
    endif()
endif()
message(STATUS "EUI render backend: requested=${EUI_RENDER_BACKEND}, resolved=${EUI_RESOLVED_RENDER_BACKEND}")

set(EUI_NEO_LIBRARY_TYPE STATIC)
if(EUI_BUILD_SHARED)
    set(EUI_NEO_LIBRARY_TYPE SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/3rd/dependencies.cmake")

find_package(Python3 COMPONENTS Interpreter QUIET)

# -----------------------------------------------------------------------------
# Core library sources
# -----------------------------------------------------------------------------

set(CORE_SOURCES
    core/platform/async.cpp
    core/platform/bundled_resources.cpp
    core/platform/json.cpp
    core/platform/network.cpp
    core/platform/native_bridge.c
    core/platform/performance_stats.cpp
    core/platform/platform.cpp
    core/render/image.cpp
    core/render/image_stream.cpp
    core/render/image_facade.cpp
    core/render/image_source.cpp
    core/render/primitive.cpp
    core/render/render_backend.cpp
    core/render/shadertoy.cpp
    core/render/shadertoy_json.cpp
    core/render/shadertoy_primitive.cpp
    core/render/stb_image_impl.cpp
    core/render/text.cpp
    core/platform/tray_bridge.c
    core/window/window_backend.cpp
    core/window/window_input_backend.cpp
)

if(EUI_WINDOW_BACKEND STREQUAL "win32")
    list(REMOVE_ITEM CORE_SOURCES core/window/window_backend.cpp core/window/window_input_backend.cpp)
    list(APPEND CORE_SOURCES core/window/win32_backend.cpp)
endif()

list(APPEND CORE_SOURCES core/audio/audio.cpp)

if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "opengl")
    list(APPEND CORE_SOURCES
        core/render/opengl/opengl_backend.cpp
        core/render/opengl/opengl_image.cpp
        core/render/opengl/opengl_primitives.cpp
        core/render/opengl/opengl_shadertoy.cpp
        core/render/opengl/opengl_text.cpp
    )
elseif(EUI_RESOLVED_RENDER_BACKEND STREQUAL "vulkan")
    list(APPEND CORE_SOURCES ${EUI_VULKAN_BACKEND_SOURCES})
elseif(EUI_RESOLVED_RENDER_BACKEND STREQUAL "d2d")
    list(APPEND CORE_SOURCES core/render/d2d/d2d_backend.cpp)
endif()

if(EUI_WINDOW_BACKEND STREQUAL "glfw")
    list(APPEND CORE_SOURCES core/platform/ime_bridge.c)
endif()

# -----------------------------------------------------------------------------
# Core library target
# -----------------------------------------------------------------------------

add_library(eui_neo ${EUI_NEO_LIBRARY_TYPE} ${CORE_SOURCES})
add_library(eui::neo ALIAS eui_neo)
set_target_properties(eui_neo PROPERTIES EXPORT_NAME neo)
if(EUI_BUILD_SHARED AND WIN32)
    set_target_properties(eui_neo PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
target_include_directories(eui_neo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${TRAY_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/eui-neo>
)
target_include_directories(eui_neo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/3rd/yyjson-0.12.0/src")
if(EUI_BUILD_NEOEDITOR_ONLY)
    target_compile_definitions(eui_neo PRIVATE EUI_PORTABLE_RUNTIME_ASSETS=1)
endif()
target_include_directories(eui_neo PRIVATE "${EUI_MINIAUDIO_DIR}")
if(UNIX AND NOT APPLE)
    target_link_libraries(eui_neo PUBLIC ${CMAKE_DL_LIBS} m)
elseif(APPLE)
    target_link_libraries(eui_neo PUBLIC
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation")
endif()
target_link_libraries(eui_neo PUBLIC Threads::Threads)
target_compile_features(eui_neo PUBLIC cxx_std_17)
target_compile_definitions(eui_neo PRIVATE YYJSON_DISABLE_WRITER=1)
# Keep application-facing debug defaults tied to CMake's configuration rather
# than compiler-specific NDEBUG flags.  FetchContent consumers inherit this
# definition through eui::neo when their target links the framework.
target_compile_definitions(eui_neo INTERFACE
    $<$<CONFIG:Debug>:EUI_DEBUG_BUILD=1>)
eui_apply_compile_options(eui_neo)

if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "vulkan")
    add_executable(eui_shadertoy_wrap
        scripts/shadertoy_wrap.cpp
        core/render/shadertoy.cpp
    )
    target_include_directories(eui_shadertoy_wrap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(eui_shadertoy_wrap PRIVATE cxx_std_17)
    eui_apply_compile_options(eui_shadertoy_wrap)
    if(MINGW)
        target_link_options(eui_shadertoy_wrap PRIVATE -municode)
    endif()
    set_target_properties(eui_shadertoy_wrap PROPERTIES
        EXPORT_NAME shadertoy_wrap)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/scripts/EuiShaderToy.cmake")

set(EUI_YYJSON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/3rd/yyjson-0.12.0/src/yyjson.c")
if(EUI_BUILD_SHARED)
    add_library(eui_yyjson STATIC "${EUI_YYJSON_SOURCE}")
    target_include_directories(eui_yyjson PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/3rd/yyjson-0.12.0/src")
    target_compile_definitions(eui_yyjson PRIVATE YYJSON_DISABLE_WRITER=1)
    set_target_properties(eui_yyjson PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    eui_silence_third_party_warnings(eui_yyjson)
    target_link_libraries(eui_neo PRIVATE eui_yyjson)
else()
    target_sources(eui_neo PRIVATE "${EUI_YYJSON_SOURCE}")
endif()

if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "opengl")
    target_compile_definitions(eui_neo PUBLIC EUI_RENDER_BACKEND_OPENGL=1)
    target_link_libraries(eui_neo PUBLIC glad OpenGL::GL)
elseif(EUI_RESOLVED_RENDER_BACKEND STREQUAL "d2d")
    target_compile_definitions(eui_neo PUBLIC EUI_RENDER_BACKEND_D2D=1)
    target_link_libraries(eui_neo PUBLIC d2d1)
elseif(EUI_RESOLVED_RENDER_BACKEND STREQUAL "vulkan")
    target_compile_definitions(eui_neo PUBLIC EUI_RENDER_BACKEND_VULKAN=1)
    if(EUI_VULKAN_LOW_LATENCY_PRESENT)
        target_compile_definitions(eui_neo PRIVATE EUI_VULKAN_LOW_LATENCY_PRESENT=1)
    endif()
    target_link_libraries(eui_neo PUBLIC Vulkan::Vulkan)
endif()

if(EUI_WINDOW_BACKEND STREQUAL "sdl2")
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 CONFIG REQUIRED)
    endif()
    target_compile_definitions(eui_neo PUBLIC EUI_WINDOW_BACKEND_SDL2=1)
    target_link_libraries(eui_neo PUBLIC SDL2::SDL2)
    set(EUI_APP_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/core/app/sdl2_app_main.cpp")
elseif(EUI_WINDOW_BACKEND STREQUAL "win32")
    target_compile_definitions(eui_neo PUBLIC EUI_WINDOW_BACKEND_WIN32=1)
    target_link_libraries(eui_neo PUBLIC gdi32)
    set(EUI_APP_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/core/app/win32_app_main.cpp")
else()
    target_link_libraries(eui_neo PUBLIC glfw)
    set(EUI_APP_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/core/app/glfw_app_main.cpp")
endif()

# App helpers can be called by a parent project after add_subdirectory(). Keep
# the generated entry source in a global property because normal CMake
# variables from this directory are not visible in the parent's call scope.
set_property(GLOBAL PROPERTY EUI_NEO_APP_MAIN_SOURCE "${EUI_APP_MAIN_SOURCE}")

target_link_libraries(eui_neo PUBLIC Freetype::Freetype)

# -----------------------------------------------------------------------------
# Platform capabilities
# -----------------------------------------------------------------------------

if(WIN32)
    target_compile_definitions(eui_neo PUBLIC EUI_TRAY_WINAPI=1 NOMINMAX)
    target_link_libraries(eui_neo PUBLIC winmm urlmon shell32 comdlg32 user32 imm32 pdh)
elseif(APPLE)
    target_compile_definitions(eui_neo PUBLIC EUI_TRAY_APPKIT=1)
    target_link_libraries(eui_neo PUBLIC "-framework Cocoa" objc)
elseif(UNIX AND EUI_ENABLE_TRAY AND EUI_GIO_FOUND)
    # StatusNotifierItem 通过 GDBus 实现 SNI 和 DBusMenu，仅依赖 glib/gio。
    target_compile_definitions(eui_neo PUBLIC EUI_TRAY_SNI=1)
    target_include_directories(eui_neo PUBLIC ${EUI_GIO_INCLUDE_DIRS})
    target_compile_options(eui_neo PRIVATE ${EUI_GIO_CFLAGS_OTHER})
    target_link_directories(eui_neo PUBLIC ${EUI_GIO_LIBRARY_DIRS})
    target_link_libraries(eui_neo PUBLIC ${EUI_GIO_LIBRARIES})
elseif(UNIX AND EUI_ENABLE_TRAY AND EUI_APPINDICATOR_FOUND AND EUI_GTK3_FOUND)
    # 未找到 glib/gio 时，使用可用的 GTK3 和 libappindicator 后端。
    target_compile_definitions(eui_neo PUBLIC EUI_TRAY_APPINDICATOR=1)
    target_include_directories(eui_neo PUBLIC ${EUI_APPINDICATOR_INCLUDE_DIRS} ${EUI_GTK3_INCLUDE_DIRS})
    target_compile_options(eui_neo PRIVATE ${EUI_APPINDICATOR_CFLAGS_OTHER} ${EUI_GTK3_CFLAGS_OTHER})
    target_link_directories(eui_neo PUBLIC ${EUI_APPINDICATOR_LIBRARY_DIRS} ${EUI_GTK3_LIBRARY_DIRS})
    target_link_libraries(eui_neo PUBLIC ${EUI_APPINDICATOR_LIBRARIES} ${EUI_GTK3_LIBRARIES})
endif()

if(TARGET CURL::libcurl)
    target_link_libraries(eui_neo PUBLIC CURL::libcurl)
    target_compile_definitions(eui_neo PUBLIC EUI_HAS_CURL=1)
endif()

if(TARGET md4c::md4c)
    target_link_libraries(eui_neo PUBLIC md4c::md4c)
    target_compile_definitions(eui_neo PUBLIC EUI_HAS_MD4C=1)
endif()

if(EUI_ENABLE_MODULES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/CMakeLists.txt")
    add_subdirectory(modules)
endif()

# -----------------------------------------------------------------------------
# App target helpers
# -----------------------------------------------------------------------------

set(EUI_APP_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(EUI_APP_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}")
set(EUI_SHADERTOY_RUNTIME_ASSET_DIR "assets/shaders/shadertoy")

set(APP_ICON_RESOURCE)
if(WIN32)
    set(APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    set(APP_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/eui_app_icon.rc")
    file(TO_NATIVE_PATH "${APP_ICON_ICO}" APP_ICON_ICO_NATIVE)
    string(REPLACE "\\" "\\\\" APP_ICON_ICO_ESCAPED "${APP_ICON_ICO_NATIVE}")
    file(WRITE "${APP_ICON_RC}" "IDI_APP_ICON ICON \"${APP_ICON_ICO_ESCAPED}\"\n")
    set(APP_ICON_RESOURCE "${APP_ICON_RC}")
endif()
set_property(GLOBAL PROPERTY EUI_NEO_APP_ICON_RESOURCE "${APP_ICON_RESOURCE}")

set(EUI_SHARED_ASSET_TARGET)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND EXISTS "${EUI_APP_ASSETS_DIR}" AND NOT EUI_BUILD_NEOEDITOR_ONLY)
    add_custom_target(eui_neo_runtime_assets
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${EUI_APP_RUNTIME_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EUI_APP_ASSETS_DIR}"
            "${EUI_APP_RUNTIME_DIR}/assets"
        VERBATIM
    )
    set(EUI_SHARED_ASSET_TARGET eui_neo_runtime_assets)
endif()
set_property(GLOBAL PROPERTY EUI_NEO_APP_ASSETS_DIR "${EUI_APP_ASSETS_DIR}")
set_property(GLOBAL PROPERTY EUI_NEO_SHARED_ASSET_TARGET "${EUI_SHARED_ASSET_TARGET}")
set_property(GLOBAL PROPERTY EUI_NEO_BUILD_SHARED "${EUI_BUILD_SHARED}")
set_property(GLOBAL PROPERTY EUI_NEO_WINDOW_BACKEND "${EUI_WINDOW_BACKEND}")
set_property(GLOBAL PROPERTY EUI_NEO_MINGW_STATIC_LINK_OPTIONS "${EUI_MINGW_STATIC_LINK_OPTIONS}")

function(eui_neo_copy_assets target_name)
    if(EUI_BUILD_NEOEDITOR_ONLY AND target_name STREQUAL "neo_editor")
        # NeoEditor standalone builds embed all product resources in the EXE.
        return()
    endif()
    get_property(_eui_shared_asset_target GLOBAL PROPERTY EUI_NEO_SHARED_ASSET_TARGET)
    get_property(_eui_app_assets_dir GLOBAL PROPERTY EUI_NEO_APP_ASSETS_DIR)
    if(_eui_shared_asset_target)
        add_dependencies(${target_name} ${_eui_shared_asset_target})
    elseif(EXISTS "${_eui_app_assets_dir}")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${target_name}>/assets"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_eui_app_assets_dir}"
                "$<TARGET_FILE_DIR:${target_name}>/assets"
            VERBATIM
        )
    endif()
endfunction()

function(eui_neo_copy_runtime_dependencies target_name)
    get_property(_eui_build_shared GLOBAL PROPERTY EUI_NEO_BUILD_SHARED)
    get_property(_eui_window_backend GLOBAL PROPERTY EUI_NEO_WINDOW_BACKEND)
    if(_eui_build_shared AND _eui_window_backend STREQUAL "glfw")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:glfw>
                $<TARGET_FILE_DIR:${target_name}>
        )
    endif()
endfunction()

function(eui_neo_copy_app_assets target_name asset_dir)
    if(EXISTS "${asset_dir}")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target_name}>/assets
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${asset_dir}" $<TARGET_FILE_DIR:${target_name}>/assets
        )
    endif()
endfunction()

function(eui_neo_configure_app target_name)
    get_property(_eui_app_main_source GLOBAL PROPERTY EUI_NEO_APP_MAIN_SOURCE)
    get_property(_eui_app_icon_resource GLOBAL PROPERTY EUI_NEO_APP_ICON_RESOURCE)
    if(NOT _eui_app_main_source)
        message(FATAL_ERROR
            "EUI app entry source is unavailable. Ensure EUI-NEO is configured "
            "before calling eui_neo_configure_app().")
    endif()
    target_sources(${target_name} PRIVATE "${_eui_app_main_source}" ${_eui_app_icon_resource})
    target_link_libraries(${target_name} PRIVATE eui::neo)
    eui_apply_compile_options(${target_name})
    eui_apply_app_link_options(${target_name})
    eui_neo_copy_assets(${target_name})
    eui_neo_copy_runtime_dependencies(${target_name})
endfunction()

function(eui_neo_configure_named_app target_name)
    if(target_name STREQUAL "shadertoy")
        set(shadertoy_demo_spirv
            "${EUI_APP_RUNTIME_DIR}/${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/demo.frag.spv")
        set(shadertoy_demo_source
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/shadertoy/demo.frag")
        set(shadertoy_presets_dir
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/shadertoy")
        target_compile_definitions(${target_name} PRIVATE
            EUI_SHADERTOY_DEMO_SOURCE="${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/demo.frag"
            EUI_SHADERTOY_DEMO_SPIRV="${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/demo.frag.spv"
            EUI_SHADERTOY_PRESETS_DIR="${EUI_SHADERTOY_RUNTIME_ASSET_DIR}")
        if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "vulkan")
            eui_compile_shadertoy(${target_name}
                SOURCE "${shadertoy_demo_source}"
                OUTPUT "${shadertoy_demo_spirv}")
            if(EXISTS "${shadertoy_presets_dir}")
                file(GLOB preset_configs CONFIGURE_DEPENDS
                     "${shadertoy_presets_dir}/*/config.json")
                foreach(preset_config IN LISTS preset_configs)
                    eui_compile_shadertoy_config(${target_name}
                        CONFIG "${preset_config}"
                        ASSET_ROOT "${shadertoy_presets_dir}"
                        OUTPUT_ROOT "${EUI_APP_RUNTIME_DIR}/${EUI_SHADERTOY_RUNTIME_ASSET_DIR}")
                endforeach()
            endif()
        endif()
    elseif(target_name STREQUAL "gallery")
        set(gallery_shadertoy_spirv
            "${EUI_APP_RUNTIME_DIR}/${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/gallery_demo.frag.spv")
        target_compile_definitions(${target_name} PRIVATE
            EUI_GALLERY_SHADERTOY_SOURCE="${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/demo.frag"
            EUI_GALLERY_SHADERTOY_NOISE="${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/blackhole/color_noise.png"
            EUI_GALLERY_SHADERTOY_SPIRV="${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/gallery_demo.frag.spv")
        if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "vulkan")
            eui_compile_shadertoy(${target_name}
                SOURCE "assets/shaders/shadertoy/demo.frag"
                OUTPUT "${gallery_shadertoy_spirv}")
        endif()
    elseif(target_name STREQUAL "promo")
        set(promo_shadertoy_spirv
            "${EUI_APP_RUNTIME_DIR}/${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/promo_demo.frag.spv")
        target_compile_definitions(${target_name} PRIVATE
            EUI_SHADERTOY_DEMO_SOURCE="${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/demo.frag"
            EUI_SHADERTOY_DEMO_SPIRV="${EUI_SHADERTOY_RUNTIME_ASSET_DIR}/promo_demo.frag.spv")
        if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "vulkan")
            eui_compile_shadertoy(${target_name}
                SOURCE "assets/shaders/shadertoy/demo.frag"
                OUTPUT "${promo_shadertoy_spirv}")
        endif()
    elseif(target_name STREQUAL "keyboard")
        target_link_libraries(${target_name} PRIVATE eui::module_keyboard)
    elseif(target_name STREQUAL "serial_tool")
        target_link_libraries(${target_name} PRIVATE eui::module_serial)
    elseif(target_name STREQUAL "vcd_viewer")
        target_link_libraries(${target_name} PRIVATE ZLIB::ZLIB)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Bundled examples and internal tests
# -----------------------------------------------------------------------------

if(EUI_BUILD_APPS)
    file(GLOB APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
    foreach(app_source ${APP_SOURCES})
        get_filename_component(app_name "${app_source}" NAME_WE)
        if(app_name STREQUAL "ffmpeg_video_player")
            message(STATUS "Skipping ffmpeg_video_player; enable EUI_BUILD_FFMPEG_VIDEO_EXAMPLE to build it.")
            continue()
        endif()
        if(app_name STREQUAL "keyboard" AND NOT TARGET eui::module_keyboard)
            message(STATUS "Skipping keyboard example because the keyboard module is not available.")
            continue()
        endif()
        if(app_name STREQUAL "serial_tool" AND NOT TARGET eui::module_serial)
            message(STATUS "Skipping serial_tool example because the serial module is not available.")
            continue()
        endif()
        add_executable(${app_name} "${app_source}")
        eui_neo_configure_app(${app_name})
        eui_neo_configure_named_app(${app_name})
    endforeach()
endif()

if(EUI_BUILD_FFMPEG_VIDEO_EXAMPLE)
    find_package(FFMPEG REQUIRED)
    if(NOT FFMPEG_FOUND OR NOT FFMPEG_INCLUDE_DIRS OR NOT FFMPEG_LIBRARIES)
        message(FATAL_ERROR
            "FFmpeg development files are required. Install ffmpeg with vcpkg and configure with its toolchain file.")
    endif()

    add_executable(ffmpeg_video_player examples/ffmpeg_video_player.cpp)
    eui_neo_configure_app(ffmpeg_video_player)
    target_include_directories(ffmpeg_video_player PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(ffmpeg_video_player PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(ffmpeg_video_player PRIVATE ${FFMPEG_LIBRARIES})
endif()

if(EUI_BUILD_USER_APPS)
    file(GLOB USER_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/apps/*.cpp")
    foreach(app_source ${USER_APP_SOURCES})
        get_filename_component(app_name "${app_source}" NAME_WE)
        if(EUI_BUILD_NEOEDITOR_ONLY AND NOT app_name STREQUAL "neo_editor")
            continue()
        endif()
        if(app_name IN_LIST EUI_USER_APP_SKIP)
            message(STATUS "Skipping user app '${app_name}' (listed in EUI_USER_APP_SKIP).")
            continue()
        endif()
        if(TARGET ${app_name})
            message(FATAL_ERROR "Duplicate EUI app target name '${app_name}'. Rename one app source or folder.")
        endif()
        if(app_name STREQUAL "serial_tool" AND NOT TARGET eui::module_serial)
            message(STATUS "Skipping serial_tool app because the serial module is not available.")
            continue()
        endif()
        add_executable(${app_name} "${app_source}")
        eui_neo_configure_app(${app_name})
        eui_neo_configure_named_app(${app_name})
    endforeach()

    file(GLOB USER_APP_ENTRY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/apps/*/app.cpp")
    foreach(app_source ${USER_APP_ENTRY_SOURCES})
        get_filename_component(app_dir "${app_source}" DIRECTORY)
        get_filename_component(app_name "${app_dir}" NAME)
        if(EUI_BUILD_NEOEDITOR_ONLY AND NOT app_name STREQUAL "neo_editor")
            continue()
        endif()
        if(app_name IN_LIST EUI_USER_APP_SKIP)
            message(STATUS "Skipping user app '${app_name}' (listed in EUI_USER_APP_SKIP).")
            continue()
        endif()
        if(TARGET ${app_name})
            message(FATAL_ERROR "Duplicate EUI app target name '${app_name}'. Rename one app source or folder.")
        endif()
        file(GLOB_RECURSE app_local_sources CONFIGURE_DEPENDS "${app_dir}/*.cpp")
        list(SORT app_local_sources)
        if(app_name STREQUAL "keyboard" AND NOT TARGET eui::module_keyboard)
            message(STATUS "Skipping keyboard app because the keyboard module is not available.")
            continue()
        endif()
        if(app_name STREQUAL "serial_tool" AND NOT TARGET eui::module_serial)
            message(STATUS "Skipping serial_tool app because the serial module is not available.")
            continue()
        endif()
        add_executable(${app_name} ${app_local_sources})
        target_include_directories(${app_name} PRIVATE "${app_dir}")
        eui_neo_configure_app(${app_name})
        eui_neo_configure_named_app(${app_name})
        if(NOT EUI_BUILD_NEOEDITOR_ONLY)
            eui_neo_copy_app_assets(${app_name} "${app_dir}/assets")
        endif()
    endforeach()

    # R2 图片粘贴：附件 PNG 编码用 libpng 简化写 API（model/png_encode.cpp）。
    if(TARGET neo_editor AND TARGET png_static)
        target_link_libraries(neo_editor PRIVATE png_static)
    endif()
    if(TARGET neo_editor)
        target_link_libraries(neo_editor PRIVATE freetype)
        target_include_directories(neo_editor PRIVATE "${NEO_EDITOR_GENERATED_INCLUDE_DIR}")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.svg" NEO_EDITOR_APP_ICON_SVG)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NeoEditorIcon.h.in"
            "${NEO_EDITOR_GENERATED_INCLUDE_DIR}/neo_editor_icon_generated.h" @ONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.svg")
        if(WIN32)
            set(NEO_EDITOR_DOCUMENT_ICONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/assets/icons")
            set(NEO_EDITOR_DOCUMENT_ICONS_RC "${CMAKE_CURRENT_BINARY_DIR}/neo_editor_document_icons.rc")
            file(TO_NATIVE_PATH "${NEO_EDITOR_DOCUMENT_ICONS_DIR}/txt.ico" NEO_EDITOR_TXT_ICON_NATIVE)
            file(TO_NATIVE_PATH "${NEO_EDITOR_DOCUMENT_ICONS_DIR}/md.ico" NEO_EDITOR_MD_ICON_NATIVE)
            file(TO_NATIVE_PATH "${NEO_EDITOR_DOCUMENT_ICONS_DIR}/code.ico" NEO_EDITOR_CODE_ICON_NATIVE)
            file(TO_NATIVE_PATH "${NEO_EDITOR_DOCUMENT_ICONS_DIR}/data.ico" NEO_EDITOR_DATA_ICON_NATIVE)
            file(TO_NATIVE_PATH "${NEO_EDITOR_DOCUMENT_ICONS_DIR}/file.ico" NEO_EDITOR_FILE_ICON_NATIVE)
            string(REPLACE "\\" "\\\\" NEO_EDITOR_TXT_ICON_ESCAPED "${NEO_EDITOR_TXT_ICON_NATIVE}")
            string(REPLACE "\\" "\\\\" NEO_EDITOR_MD_ICON_ESCAPED "${NEO_EDITOR_MD_ICON_NATIVE}")
            string(REPLACE "\\" "\\\\" NEO_EDITOR_CODE_ICON_ESCAPED "${NEO_EDITOR_CODE_ICON_NATIVE}")
            string(REPLACE "\\" "\\\\" NEO_EDITOR_DATA_ICON_ESCAPED "${NEO_EDITOR_DATA_ICON_NATIVE}")
            string(REPLACE "\\" "\\\\" NEO_EDITOR_FILE_ICON_ESCAPED "${NEO_EDITOR_FILE_ICON_NATIVE}")
            file(WRITE "${NEO_EDITOR_DOCUMENT_ICONS_RC}"
                "IDI_APP_ICON ICON \"${APP_ICON_ICO_ESCAPED}\"\n102 ICON \"${NEO_EDITOR_TXT_ICON_ESCAPED}\"\n103 ICON \"${NEO_EDITOR_MD_ICON_ESCAPED}\"\n104 ICON \"${NEO_EDITOR_CODE_ICON_ESCAPED}\"\n105 ICON \"${NEO_EDITOR_DATA_ICON_ESCAPED}\"\n106 ICON \"${NEO_EDITOR_FILE_ICON_ESCAPED}\"\n")
            file(READ "${NEO_EDITOR_VERSION_RC}" NEO_EDITOR_VERSION_RC_CONTENT)
            file(APPEND "${NEO_EDITOR_DOCUMENT_ICONS_RC}" "${NEO_EDITOR_VERSION_RC_CONTENT}")
            set(NEO_EDITOR_BUNDLED_LICENSES "${CMAKE_CURRENT_BINARY_DIR}/neo_editor_bundled_licenses.txt")
            file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" NEO_EDITOR_MAIN_LICENSE_TEXT)
            file(READ "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/THIRD-PARTY-NOTICES.md"
                NEO_EDITOR_THIRD_PARTY_NOTICES_TEXT)
            string(CONCAT NEO_EDITOR_BUNDLED_LICENSES_TEXT
                "EUI-Edits ${NEO_EDITOR_VERSION} - bundled licenses and notices\n\n"
                "==================== PROJECT LICENSE ====================\n\n"
                "${NEO_EDITOR_MAIN_LICENSE_TEXT}\n\n"
                "==================== THIRD-PARTY NOTICES ====================\n\n"
                "${NEO_EDITOR_THIRD_PARTY_NOTICES_TEXT}\n\n")
            file(GLOB NEO_EDITOR_LICENSE_FILES CONFIGURE_DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/LICENSES/*.txt")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/THIRD-PARTY-NOTICES.md"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/Font Awesome 7 Free-Solid-900.otf"
                ${NEO_EDITOR_LICENSE_FILES})
            foreach(NEO_EDITOR_LICENSE_FILE IN LISTS NEO_EDITOR_LICENSE_FILES)
                get_filename_component(NEO_EDITOR_LICENSE_NAME "${NEO_EDITOR_LICENSE_FILE}" NAME)
                file(READ "${NEO_EDITOR_LICENSE_FILE}" NEO_EDITOR_LICENSE_TEXT)
                string(APPEND NEO_EDITOR_BUNDLED_LICENSES_TEXT
                    "\n\n==================== ${NEO_EDITOR_LICENSE_NAME} ====================\n\n"
                    "${NEO_EDITOR_LICENSE_TEXT}")
            endforeach()
            file(WRITE "${NEO_EDITOR_BUNDLED_LICENSES}" "${NEO_EDITOR_BUNDLED_LICENSES_TEXT}")
            file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/Font Awesome 7 Free-Solid-900.otf"
                NEO_EDITOR_FONT_AWESOME_NATIVE)
            file(TO_NATIVE_PATH "${NEO_EDITOR_BUNDLED_LICENSES}" NEO_EDITOR_LICENSE_BUNDLE_NATIVE)
            string(REPLACE "\\" "\\\\" NEO_EDITOR_FONT_AWESOME_ESCAPED "${NEO_EDITOR_FONT_AWESOME_NATIVE}")
            string(REPLACE "\\" "\\\\" NEO_EDITOR_LICENSE_BUNDLE_ESCAPED "${NEO_EDITOR_LICENSE_BUNDLE_NATIVE}")
            file(APPEND "${NEO_EDITOR_DOCUMENT_ICONS_RC}"
                "501 RCDATA \"${NEO_EDITOR_FONT_AWESOME_ESCAPED}\"\n"
                "502 RCDATA \"${NEO_EDITOR_LICENSE_BUNDLE_ESCAPED}\"\n")
            set_property(SOURCE "${NEO_EDITOR_DOCUMENT_ICONS_RC}" APPEND PROPERTY OBJECT_DEPENDS
                "${NEO_EDITOR_BUNDLED_LICENSES};${CMAKE_CURRENT_SOURCE_DIR}/assets/Font Awesome 7 Free-Solid-900.otf")
            target_compile_definitions(neo_editor PRIVATE EUI_EDITS_BUNDLED_RESOURCES=1)
            # Compile the app icon and five document groups together: separate RC files restart their
            # generated RT_ICON IDs at 1 and collide at link time.
            get_target_property(NEO_EDITOR_SOURCES neo_editor SOURCES)
            list(REMOVE_ITEM NEO_EDITOR_SOURCES "${APP_ICON_RESOURCE}")
            set_property(TARGET neo_editor PROPERTY SOURCES "${NEO_EDITOR_SOURCES}")
            target_sources(neo_editor PRIVATE "${NEO_EDITOR_DOCUMENT_ICONS_RC}")
        endif()
    endif()
endif()

if(EUI_BUILD_TEST_FIXTURES)
    enable_testing()

    file(GLOB TEST_FIXTURE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*.cpp")
    foreach(fixture_source ${TEST_FIXTURE_SOURCES})
        get_filename_component(fixture_name "${fixture_source}" NAME_WE)
        if(EUI_WINDOW_BACKEND STREQUAL "win32" AND fixture_name STREQUAL "window_controls_viewer")
            continue() # This fixture calls SDL/GLFW window APIs directly.
        endif()
        add_executable(${fixture_name} "${fixture_source}")
        eui_neo_configure_app(${fixture_name})
    endforeach()

    file(GLOB TEST_PROBE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/probes/*.cpp")
    foreach(probe_source ${TEST_PROBE_SOURCES})
        get_filename_component(probe_name "${probe_source}" NAME_WE)
        if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "d2d" AND
           (probe_name STREQUAL "gpu_image_probe" OR probe_name STREQUAL "shadertoy_probe" OR probe_name STREQUAL "shadertoy_runtime_probe"))
            continue() # These probes require APIs absent from the experimental D2D renderer.
        endif()
        if(probe_name STREQUAL "d2d_render_probe" AND NOT EUI_RESOLVED_RENDER_BACKEND STREQUAL "d2d")
            continue()
        endif()
        add_executable(${probe_name} "${probe_source}")
        target_link_libraries(${probe_name} PRIVATE eui::neo)
        eui_apply_compile_options(${probe_name})
        eui_neo_copy_assets(${probe_name})
        eui_neo_copy_runtime_dependencies(${probe_name})
        if(MSVC)
            target_link_options(${probe_name} PRIVATE /SUBSYSTEM:CONSOLE)
        elseif(MINGW)
            target_link_options(${probe_name} PRIVATE -mconsole)
        endif()
        target_compile_definitions(${probe_name} PRIVATE EUI_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME ${probe_name} COMMAND ${probe_name} --frames 3)
        set_tests_properties(${probe_name} PROPERTIES LABELS "probe")
        if(probe_name STREQUAL "gpu_image_probe")
            set_tests_properties(${probe_name} PROPERTIES FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
        endif()
    endforeach()

    if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "vulkan" AND TARGET shadertoy_probe)
        set(shadertoy_probe_assets_dir
            "${CMAKE_CURRENT_BINARY_DIR}/shadertoy-test-assets")
        eui_compile_shadertoy(shadertoy_probe
            SOURCE "tests/assets/shadertoy/feedback.frag"
            OUTPUT "${shadertoy_probe_assets_dir}/feedback.frag.spv"
        )
        eui_compile_shadertoy(shadertoy_probe
            SOURCE "tests/assets/shadertoy/buffer.frag"
            OUTPUT "${shadertoy_probe_assets_dir}/buffer.frag.spv"
            UNIFORMS "uValue:float"
        )
        eui_compile_shadertoy(shadertoy_probe
            SOURCE "tests/assets/shadertoy/channels.frag"
            OUTPUT "${shadertoy_probe_assets_dir}/channels.frag.spv"
            UNIFORMS "uValue:float"
        )
        eui_compile_shadertoy(shadertoy_probe
            SOURCE "tests/assets/shadertoy/pass_contract.frag"
            OUTPUT "${shadertoy_probe_assets_dir}/pass_contract.frag.spv"
        )
        set(EUI_SHADERTOY_INLINE_TEST_SOURCE
            "void mainImage(out vec4 color, in vec2 coord) { color = vec4(0.3, 0.4, 0.5, 1.0); }")
        eui_compile_shadertoy_inline(shadertoy_probe
            CONTENT "${EUI_SHADERTOY_INLINE_TEST_SOURCE}"
            OUTPUT "${shadertoy_probe_assets_dir}/inline.frag.spv"
        )
        set(EUI_SHADERTOY_HOT_RELOAD_INITIAL_SOURCE
            "void mainImage(out vec4 color, in vec2 coord) { color = vec4(0.2, 0.0, 0.0, 1.0); }")
        eui_compile_shadertoy_inline(shadertoy_probe
            CONTENT "${EUI_SHADERTOY_HOT_RELOAD_INITIAL_SOURCE}"
            OUTPUT "${shadertoy_probe_assets_dir}/hot_reload_initial.frag.spv"
        )
        set(EUI_SHADERTOY_HOT_RELOAD_RECOVERED_SOURCE
            "void mainImage(out vec4 color, in vec2 coord) { color = vec4(0.7, 0.0, 0.0, 1.0); }")
        eui_compile_shadertoy_inline(shadertoy_probe
            CONTENT "${EUI_SHADERTOY_HOT_RELOAD_RECOVERED_SOURCE}"
            OUTPUT "${shadertoy_probe_assets_dir}/hot_reload_recovered.frag.spv"
        )
        target_compile_definitions(shadertoy_probe PRIVATE
            EUI_TEST_SHADERS_DIR="${shadertoy_probe_assets_dir}")
    endif()

    file(GLOB TEST_UNIT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")
    foreach(unit_source ${TEST_UNIT_SOURCES})
        get_filename_component(unit_name "${unit_source}" NAME_WE)
        if(unit_name STREQUAL "portable_resource_paths" AND (NOT WIN32 OR EUI_BUILD_SHARED))
            # The relocation probe stubs the tray boundary of the static library.
            continue()
        endif()
        if((unit_name STREQUAL "win32_input" OR unit_name STREQUAL "dsl_main_handle") AND NOT EUI_WINDOW_BACKEND STREQUAL "win32")
            continue()
        endif()
        if(unit_name STREQUAL "shadertoy_packaged_assets" AND
           EUI_RESOLVED_RENDER_BACKEND STREQUAL "d2d")
            # 打包的 shader 资源随渲染后端一起产出，D2D 实验版不带 shadertoy
            # （探针那侧已有同样的跳过，见上面 shadertoy_probe 分支）。
            continue()
        endif()
        add_executable(${unit_name} "${unit_source}")
        target_link_libraries(${unit_name} PRIVATE eui::neo)
        target_include_directories(${unit_name} PRIVATE "${NEO_EDITOR_GENERATED_INCLUDE_DIR}")
        eui_apply_compile_options(${unit_name})
        if(unit_name STREQUAL "lp_decorations")
            # Live Preview 适配层的测试要同时编进 app 侧的解析器（lp_plan.cpp）。
            # 测的是编辑器真正跑的那条路：lp_plan → lp_decorations → 组件装饰。
            # text_file.cpp 提供路径的 UTF-8 ↔ 宽字符转换（5a644a2 起
            # resolveLocalImageSrc 依赖它，不编进来就断链）。
            # text_file.cpp 的 save 走 atomic_write（编码严格往返），一并编入。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/lp_plan.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "file_assoc_registration" OR unit_name STREQUAL "source_decorations" OR unit_name STREQUAL "settings_confirmation" OR unit_name STREQUAL "i18n" OR unit_name STREQUAL "menu_commands")
            target_include_directories(${unit_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "file_assoc_registration")
            # 测试直编 file_assoc.cpp；其中的写能力登记走 pathFromUtf8，需带上
            # text_file.cpp（其 save 又依赖 atomic_write.cpp）。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
        endif()
        if(unit_name STREQUAL "font_safety")
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/font_safety.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/settings.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
            target_link_libraries(${unit_name} PRIVATE freetype)
        endif()
        if(unit_name STREQUAL "settings_atomic")
            # T9：settings/recovery 原子写。测试直接编入 app 侧的 settings.cpp 及其
            # 依赖：text_file.cpp 提供路径 UTF-8 ↔ 原生转换（settings 走它的约定），
            # atomic_write.cpp 是被测的原子写 helper（唯一临时文件 + 替换 + 故障注入点）。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/settings.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "vault_watcher")
            # T18 并发压测：直接编 vault_watcher.cpp（应用侧文件，不在 eui::neo 里），
            # text_file.cpp 提供 pathFromUtf8（Windows 下 run() 开目录句柄要用，与
            # lp_decorations 同款接线）。再定义 NEO_VAULT_WATCHER_TEST_HOOKS 打开
            # fireChangeForTest 测试钩子——非 Windows 分支没有监视线程，没有钩子就
            # 灌不进事件，onChange/bump 的并发测不到。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/vault_watcher.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
            target_compile_definitions(${unit_name} PRIVATE NEO_VAULT_WATCHER_TEST_HOOKS=1)
        endif()
        if(unit_name STREQUAL "undo_incremental")
            # T3/T11：增量撤销 + 组件 loadDocument 的对照测试要 include 应用层的
            # state/app_state.h（applyInlineFormat 的"两点修改"、旧 reset 的逐字段复刻对照）。
            # applyEditorCommand 也分发 AI 清理，需链接应用层纯文本实现。
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/clean_ai.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/clipboard_image.cpp")
        endif()
        if(unit_name STREQUAL "clean_ai")
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/clean_ai.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/clipboard_image.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "fold_state" OR unit_name STREQUAL "find_actions")
            # R8：折叠键对账与"查找命中展开祖先"的入口逻辑在应用层内联头里
            # （state/app_state.h 的 syncFoldsWithEditorText、ui/find_bar.h 的
            # selectFindMatch），所以测试要拿到 apps/neo_editor 的 include 目录，
            # 并编入 lp_plan.cpp（章节表 / 父链字段的唯一实现）。
            # text_file.cpp + atomic_write.cpp 是 app_state.h 那条依赖链的既有接线。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/lp_plan.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "lp_edit_benchmark")
            # B3 前置：真实 LP 编辑链基准（cachedPlan → cachedDecorations → 排版）。
            # 与 lp_decorations 同一套接线：要编进 app 侧解析器 lp_plan.cpp，
            # text_file.cpp / atomic_write.cpp 是 lp_decorations 依赖链的既有接线。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/lp_plan.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "image_attach")
            # R2：DIB 解析（平台文件）+ PNG 编码（libpng）+ 附件目录/命名纯逻辑。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/clipboard_image.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/png_encode.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/attachment.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor"
                "${CMAKE_CURRENT_SOURCE_DIR}")
            if(TARGET png_static)
                target_link_libraries(${unit_name} PRIVATE png_static)
            endif()
        endif()
        if(unit_name STREQUAL "text_file_encoding")
            # R4 编码严格往返：text_file.cpp（被测）+ atomic_write.cpp（save 的
            # 原子替换路径，含故障注入点）。settings.cpp 不编入——本测试不触配置。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "file_operations")
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/file_operations.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "vault_rename")
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/vault_rename.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "vault_scan_scheduler")
            # 阶段 B：按根共享扫描缓存 + 协调器（纯模块 + 真实 vault::scan）。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/vault_cache.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/tabs_trace.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/vault.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "session_write_scheduler")
            # 阶段 E：串行后台写协调器 + 真实 sessionstorage（内容寻址 body）。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/session_writer.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/tabs_trace.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/session_storage.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/settings.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "file_check_scheduler")
            # 阶段 D：后台文件核验调度（纯模块 + 真实 inspect/load）。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/file_check.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "tab_presentation")
            # 标签展示身份/调色/槽位分配的纯逻辑：只编 tab_presentation.cpp 与它的路径转换
            # 依赖（text_file.cpp 提供 pathFromUtf8/pathToUtf8），不拉整应用。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/tab_presentation.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "session_storage")
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/session_storage.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/settings.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "vault_delete_state" OR unit_name STREQUAL "file_safety" OR unit_name STREQUAL "document_tabs"
           OR unit_name STREQUAL "document_tab_cache" OR unit_name STREQUAL "document_activation_check")
            # Exercise real action dispatch with test doubles for Shell/dialogs/settings.
            # app_actions 引用 png_encode/attachment/clipboard_image（图片粘贴路径），一并编入。
            # 切页/缓存/后台任务相关的应用侧模块（阶段 B～E）也必须编入：document_tabs 与
            # app_actions 现在引用 vault_cache/file_check/session_writer/tabs_trace。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/vault_rename.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/app_actions.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/document_tabs.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/vault_cache.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/file_check.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/session_writer.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/state/tabs_trace.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/session_storage.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/vault.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/lp_plan.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/clean_ai.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/attachment.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/png_encode.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/tab_presentation.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/platform/clipboard_image.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
            if(TARGET png_static)
                target_link_libraries(${unit_name} PRIVATE png_static)
            endif()
        endif()
        if(unit_name STREQUAL "theme_loader")
            # T13 第一阶段：主题 JSON（内置导出→重载逐字段 golden / 缺字段回退 /
            # 坏 JSON / Unicode 路径 / 静态配色缓存真实失效 / last_theme_file 换进程
            # 重读）。直接编入 app 侧的 theme_loader.cpp 与 settings.cpp 及其依赖：
            # text_file.cpp 提供 pathFromUtf8（UTF-8 路径读主题与配置），
            # atomic_write.cpp 是 settings 的原子写。style_schema.h 纯头文件不接线。
            target_sources(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/theme_loader.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/obsidian_theme.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/text_file.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/settings.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor/model/atomic_write.cpp")
            target_include_directories(${unit_name} PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/apps/neo_editor")
        endif()
        if(unit_name STREQUAL "shadertoy_packaged_assets")
            eui_neo_copy_assets(${unit_name})
            if(TARGET shadertoy)
                add_dependencies(${unit_name} shadertoy)
                if(EUI_RESOLVED_RENDER_BACKEND STREQUAL "vulkan")
                    target_compile_definitions(${unit_name} PRIVATE
                        EUI_TEST_EXPECT_SHADERTOY_SPIRV=1)
                endif()
            endif()
        endif()
        add_test(NAME ${unit_name} COMMAND ${unit_name})
        if(unit_name STREQUAL "settings_atomic")
            foreach(language_mode zh en invalid legacy)
                add_test(NAME settings_language_${language_mode} COMMAND ${unit_name} --${language_mode})
                set_tests_properties(settings_language_${language_mode} PROPERTIES LABELS "unit")
            endforeach()
        endif()
        if(unit_name STREQUAL "perf_benchmark" OR unit_name STREQUAL "lp_edit_benchmark"
           OR unit_name STREQUAL "table_layout_benchmark")
            # T12 / B3 无头性能基准：只打印 ms/次，不因绝对耗时失败；仅 NEO_PERF_GUARD=1
            # 才启用相对护栏。挂 performance 标签 → 默认 `ctest -L unit`（CI/release
            # 两处都按 -L unit 跑）不会带上它，需要时单独 `ctest -L performance`。
            set_tests_properties(${unit_name} PROPERTIES LABELS "performance")
        else()
            set_tests_properties(${unit_name} PROPERTIES LABELS "unit")
        endif()
    endforeach()
endif()

if(Python3_Interpreter_FOUND)
    add_custom_target(eui_check_vulkan_shaders
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_vulkan_shaders.py" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking generated Vulkan shader headers"
        VERBATIM
    )
endif()

# -----------------------------------------------------------------------------
# Install and CMake package export
# -----------------------------------------------------------------------------

if(EUI_ENABLE_INSTALL)
    set(EUI_NEO_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/EuiNeo" CACHE STRING "Install path for EUI-NEO CMake package files")

    install(TARGETS eui_neo
        EXPORT EuiNeoTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES "${EUI_APP_MAIN_SOURCE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/eui-neo/app"
        RENAME app_main.cpp
    )
    install(DIRECTORY assets/
        DESTINATION "${CMAKE_INSTALL_DATADIR}/eui-neo/assets"
    )
    if(TARGET eui_shadertoy_wrap)
        install(TARGETS eui_shadertoy_wrap
            EXPORT EuiNeoTargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES scripts/EuiShaderToy.cmake scripts/generate_shadertoy_spirv.py
            DESTINATION ${EUI_NEO_INSTALL_CMAKEDIR})
    endif()
    set(EUI_NEO_INSTALL_DEP_TARGETS freetype png_static eui_zlib eui_md4c SDL2-static)
    if(EUI_OWNS_GLAD_TARGET)
        list(APPEND EUI_NEO_INSTALL_DEP_TARGETS glad)
    endif()
    if(EUI_OWNS_GLFW_TARGET)
        list(APPEND EUI_NEO_INSTALL_DEP_TARGETS glfw)
    endif()
    foreach(optional_target ${EUI_NEO_INSTALL_DEP_TARGETS})
        if(TARGET ${optional_target})
            install(TARGETS ${optional_target}
                EXPORT EuiNeoTargets
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            )
        endif()
    endforeach()

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
        PATTERN "detail" EXCLUDE
    )
    install(FILES include/eui/detail/dsl_app_impl.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eui/detail
    )
    set(EUI_NEO_INSTALL_HEADER_DIRS components core)
    if(EUI_ENABLE_MODULES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules")
        list(APPEND EUI_NEO_INSTALL_HEADER_DIRS modules)
    endif()
    install(DIRECTORY ${EUI_NEO_INSTALL_HEADER_DIRS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eui-neo
        FILES_MATCHING PATTERN "*.h"
    )
    if(EUI_OWNS_GLFW_TARGET AND TARGET glfw)
        install(DIRECTORY "${EUI_GLFW_DIR}/include/GLFW"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h"
        )
    endif()
    if(TARGET freetype)
        install(DIRECTORY "${EUI_FREETYPE_DIR}/include/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/freetype2
            FILES_MATCHING PATTERN "*.h"
        )
    endif()
    if(TARGET png_static)
        install(FILES
            "${EUI_LIBPNG_DIR}/png.h"
            "${EUI_LIBPNG_DIR}/pngconf.h"
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/libpng-bundled-build/pnglibconf.h"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libpng16
        )
    endif()
    install(FILES
        3rd/stb_image.h
        3rd/nanosvg.h
        3rd/nanosvgrast.h
        "${EUI_MINIAUDIO_DIR}/miniaudio.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eui-neo/3rd
    )
    install(FILES "${TRAY_INCLUDE_DIR}/tray.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eui-neo/3rd/tray
    )
    if(EUI_OWNS_GLAD_TARGET AND TARGET glad)
        install(DIRECTORY "${glad_SOURCE_DIR}/include/"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eui-neo/3rd/glad/include
            FILES_MATCHING PATTERN "*.h"
        )
    endif()
    install(FILES
        "${EUI_ZLIB_DIR}/zconf.h"
        "${EUI_ZLIB_DIR}/zlib.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eui-neo/3rd/zlib
    )
    if(TARGET eui_md4c)
        install(FILES "${EUI_MD4C_DIR}/src/md4c.h"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eui-neo/3rd/md4c/src
        )
    endif()

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/EuiNeoConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    set(EUI_NEO_CONFIG_TEMPLATE "${CMAKE_CURRENT_BINARY_DIR}/EuiNeoConfig.cmake.in")
    string(JOIN " " EUI_MINGW_STATIC_LINK_OPTIONS_CONFIG
        ${EUI_MINGW_STATIC_LINK_OPTIONS})
    file(WRITE "${EUI_NEO_CONFIG_TEMPLATE}" [=[
@PACKAGE_INIT@

include(CMakeFindDependencyMacro)

set(EUI_NEO_WINDOW_BACKEND "@EUI_WINDOW_BACKEND@")
set(EUI_NEO_RENDER_BACKEND "@EUI_RESOLVED_RENDER_BACKEND@")
set(EUI_NEO_BUILD_SHARED @EUI_BUILD_SHARED@)
set(EUI_NEO_DEPS_MODE "@EUI_DEPS_MODE@")
set(EUI_NEO_ENABLE_MARKDOWN @EUI_ENABLE_MARKDOWN@)
set(EUI_NEO_ENABLE_MODULES @EUI_ENABLE_MODULES@)
set(EUI_APP_MAIN_SOURCE "${PACKAGE_PREFIX_DIR}/@CMAKE_INSTALL_DATADIR@/eui-neo/app/app_main.cpp")
set(EUI_APP_ASSETS_DIR "${PACKAGE_PREFIX_DIR}/@CMAKE_INSTALL_DATADIR@/eui-neo/assets")
set_property(GLOBAL PROPERTY EUI_NEO_APP_MAIN_SOURCE "${EUI_APP_MAIN_SOURCE}")
set_property(GLOBAL PROPERTY EUI_NEO_APP_ASSETS_DIR "${EUI_APP_ASSETS_DIR}")

find_dependency(Threads)

if(EUI_NEO_RENDER_BACKEND STREQUAL "opengl")
    find_dependency(OpenGL)
    if(NOT @EUI_OWNS_GLAD_TARGET@)
        find_dependency(glad CONFIG)
    endif()
elseif(EUI_NEO_RENDER_BACKEND STREQUAL "vulkan")
    find_dependency(Vulkan)
endif()

if(EUI_NEO_WINDOW_BACKEND STREQUAL "glfw" AND NOT @EUI_OWNS_GLFW_TARGET@)
    find_dependency(glfw3 CONFIG)
endif()

if(EUI_NEO_WINDOW_BACKEND STREQUAL "sdl2")
    find_dependency(SDL2 CONFIG)
endif()

if(NOT WIN32)
    find_dependency(CURL)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/EuiNeoTargets.cmake")

function(eui_neo_copy_assets target_name)
    get_property(_eui_app_assets_dir GLOBAL PROPERTY EUI_NEO_APP_ASSETS_DIR)
    if(EXISTS "${_eui_app_assets_dir}")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${target_name}>/assets"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_eui_app_assets_dir}"
                "$<TARGET_FILE_DIR:${target_name}>/assets"
            VERBATIM
        )
    endif()
endfunction()

function(eui_neo_configure_app target_name)
    get_property(_eui_app_main_source GLOBAL PROPERTY EUI_NEO_APP_MAIN_SOURCE)
    if(NOT _eui_app_main_source)
        message(FATAL_ERROR
            "EUI app entry source is unavailable. Ensure EuiNeo is found "
            "before calling eui_neo_configure_app().")
    endif()
    target_sources(${target_name} PRIVATE "${_eui_app_main_source}")
    target_link_libraries(${target_name} PRIVATE eui::neo)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8)
        target_link_options(${target_name} PRIVATE /ENTRY:mainCRTStartup /SUBSYSTEM:WINDOWS)
    elseif(MINGW)
        target_link_options(${target_name} PRIVATE -mwindows)
        if(NOT EUI_NEO_BUILD_SHARED)
            target_link_options(${target_name} PRIVATE @EUI_MINGW_STATIC_LINK_OPTIONS_CONFIG@)
        endif()
    endif()
    eui_neo_copy_assets(${target_name})
endfunction()

if(EUI_NEO_RENDER_BACKEND STREQUAL "vulkan")
    include("${CMAKE_CURRENT_LIST_DIR}/EuiShaderToy.cmake")
endif()

check_required_components(EuiNeo)
]=])

    configure_package_config_file(
        "${EUI_NEO_CONFIG_TEMPLATE}"
        "${CMAKE_CURRENT_BINARY_DIR}/EuiNeoConfig.cmake"
        INSTALL_DESTINATION "${EUI_NEO_INSTALL_CMAKEDIR}"
    )

    install(EXPORT EuiNeoTargets
        FILE EuiNeoTargets.cmake
        NAMESPACE eui::
        DESTINATION "${EUI_NEO_INSTALL_CMAKEDIR}"
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/EuiNeoConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/EuiNeoConfigVersion.cmake"
        DESTINATION "${EUI_NEO_INSTALL_CMAKEDIR}"
    )
endif()
