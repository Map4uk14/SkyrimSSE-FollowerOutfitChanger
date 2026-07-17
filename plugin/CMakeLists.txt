cmake_minimum_required(VERSION 3.21)

# Only CommonLibSSE-NG is needed. The UI is PrismaUI, bound at runtime through a
# single header (PrismaUI_API.h -> GetProcAddress on PrismaUI.dll) - nothing to
# link and no imgui.dll, so there are no load-order / error-126 / UI-context
# problems to manage here.
if(NOT DEFINED ENV{COMMONLIB_SSE_FOLDER})
    message(FATAL_ERROR "Missing COMMONLIB_SSE_FOLDER environment variable")
endif()

project(FollowerOutfitChanger VERSION 1.2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# --- CommonLibSSE-NG (built from a local clone) ---
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("$ENV{COMMONLIB_SSE_FOLDER}" ${CMAKE_CURRENT_BINARY_DIR}/_deps/clib-build)
get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)

find_path(CLIB_UTIL_INCLUDE_DIRS "ClibUtil/utils.hpp")

file(GLOB_RECURSE sources ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

include(${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake)
add_commonlibsse_plugin(${PROJECT_NAME} AUTHOR "Mark" SOURCES ${sources})
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CLIB_UTIL_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# --- copy the built DLL + PrismaUI view into the game's Data folder ---
# PrismaUI resolves CreateView() paths relative to Data/PrismaUI/views/, so the
# HTML must live at Data/PrismaUI/views/FollowerOutfitChanger/index.html.
if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{SKYRIM_FOLDER}/Data/SKSE/Plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "$ENV{SKYRIM_FOLDER}/Data/SKSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$ENV{SKYRIM_FOLDER}/Data/PrismaUI/views/FollowerOutfitChanger"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/view/index.html"
            "$ENV{SKYRIM_FOLDER}/Data/PrismaUI/views/FollowerOutfitChanger/index.html"
        VERBATIM)
endif()
