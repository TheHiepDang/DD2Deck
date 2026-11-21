cmake_minimum_required(VERSION 3.21)
project(dd2_foveated_vrs VERSION 0.4.8 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT (MSVC OR MINGW))
    message(FATAL_ERROR "Use x64 MSVC or an x86_64-w64-mingw32 toolchain.")
endif()

include(FetchContent)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    # MinHook 1.3.4 still declares compatibility with CMake 3.0.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(minhook nlohmann_json)

add_library(dd2_foveated_vrs SHARED
    src/plugin.cpp
    src/graphics.cpp
    src/engine_optimizer.cpp
    src/platform.c)
if(MINGW)
    enable_language(ASM)
    target_sources(dd2_foveated_vrs PRIVATE src/guard_mingw.S)
    target_link_options(dd2_foveated_vrs PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(MSVC)
    target_compile_options(dd2_foveated_vrs PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHa> /W4 /wd4191)
endif()

target_compile_features(dd2_foveated_vrs PRIVATE cxx_std_20)
target_compile_definitions(dd2_foveated_vrs PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(dd2_foveated_vrs SYSTEM PRIVATE vendor/reframework/include)
target_link_libraries(dd2_foveated_vrs PRIVATE minhook nlohmann_json::nlohmann_json d3d12 dxgi dxguid)
set_target_properties(dd2_foveated_vrs PROPERTIES PREFIX "" CXX_EXTENSIONS OFF)

install(TARGETS dd2_foveated_vrs RUNTIME DESTINATION reframework/plugins COMPONENT Runtime)
