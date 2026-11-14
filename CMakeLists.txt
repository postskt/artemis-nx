cmake_minimum_required(VERSION 3.20)
project(artc_runner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(ARTC_BUILD_SMOKE "Build the desktop core smoke test" ON)
option(ARTC_BUILD_PLATFORM "Build the SDL2 platform layer (Step 3)" OFF)

set(ARTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(ARTC_THIRD_PARTY "${ARTC_ROOT}/third_party")

# ---- Lua 5.4.8: core interpreter + safe standard libraries only -------------
set(LUA_SOURCES
    ${ARTC_THIRD_PARTY}/lua/lapi.c
    ${ARTC_THIRD_PARTY}/lua/lcode.c
    ${ARTC_THIRD_PARTY}/lua/lctype.c
    ${ARTC_THIRD_PARTY}/lua/ldebug.c
    ${ARTC_THIRD_PARTY}/lua/ldo.c
    ${ARTC_THIRD_PARTY}/lua/ldump.c
    ${ARTC_THIRD_PARTY}/lua/lfunc.c
    ${ARTC_THIRD_PARTY}/lua/lgc.c
    ${ARTC_THIRD_PARTY}/lua/llex.c
    ${ARTC_THIRD_PARTY}/lua/lmem.c
    ${ARTC_THIRD_PARTY}/lua/lobject.c
    ${ARTC_THIRD_PARTY}/lua/lopcodes.c
    ${ARTC_THIRD_PARTY}/lua/lparser.c
    ${ARTC_THIRD_PARTY}/lua/lstate.c
    ${ARTC_THIRD_PARTY}/lua/lstring.c
    ${ARTC_THIRD_PARTY}/lua/ltable.c
    ${ARTC_THIRD_PARTY}/lua/ltm.c
    ${ARTC_THIRD_PARTY}/lua/lundump.c
    ${ARTC_THIRD_PARTY}/lua/lvm.c
    ${ARTC_THIRD_PARTY}/lua/lzio.c
    ${ARTC_THIRD_PARTY}/lua/lauxlib.c
    ${ARTC_THIRD_PARTY}/lua/lbaselib.c
    ${ARTC_THIRD_PARTY}/lua/lmathlib.c
    ${ARTC_THIRD_PARTY}/lua/lstrlib.c
    ${ARTC_THIRD_PARTY}/lua/ltablib.c
    ${ARTC_THIRD_PARTY}/lua/lutf8lib.c
)

add_library(artc_lua STATIC ${LUA_SOURCES})
target_include_directories(artc_lua PUBLIC ${ARTC_THIRD_PARTY}/lua)
set_target_properties(artc_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- Pure C++ core: no SDL include is allowed in this directory ------------
set(ARTC_CORE_SOURCES
    ${ARTC_ROOT}/src/core/Config.cpp
    ${ARTC_ROOT}/src/core/LuaLoader.cpp
    ${ARTC_ROOT}/src/core/AssetManager.cpp
    ${ARTC_ROOT}/src/core/Engine.cpp
)

add_library(artc_core STATIC ${ARTC_CORE_SOURCES})
target_include_directories(artc_core
    PUBLIC ${ARTC_ROOT}/src/core
    PUBLIC ${ARTC_THIRD_PARTY}
)
target_link_libraries(artc_core PUBLIC artc_lua)
if(MSVC)
    target_compile_options(artc_core PRIVATE /utf-8)
    if(ARTC_BUILD_SMOKE)
        add_compile_options(/utf-8)
    endif()
endif()

if(ARTC_BUILD_SMOKE)
    add_executable(artc_core_smoke ${ARTC_ROOT}/tools/core_smoke.cpp)
    target_link_libraries(artc_core_smoke PRIVATE artc_core)
endif()

# ---- Platform layer placeholder -------------------------------------------
# Step 3 adds SDL2/Input/Renderer/Audio under src/platform.  The core target
# deliberately has no SDL dependency, so this section is opt-in.
if(ARTC_BUILD_PLATFORM)
    add_subdirectory(src/platform)
endif()

# ---- Nintendo Switch notes -------------------------------------------------
# Configure with:
#   cmake -B build-switch \
#     -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
#     -DARTC_BUILD_SMOKE=OFF -DARTC_BUILD_PLATFORM=ON
# The platform CMakeLists will use devkitPro's SDL2 port and elf2nro.
if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    target_compile_definitions(artc_core PUBLIC __SWITCH__=1)
endif()
