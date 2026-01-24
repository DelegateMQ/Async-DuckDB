# Specify the minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Project name and language
project(Async-DuckDB VERSION 1.0 LANGUAGES C CXX)

# ------------------------------------------------------------------------------
# 1. DelegateMQ Configuration
# ------------------------------------------------------------------------------
set (DMQ_ALLOCATOR "OFF")
set (DMQ_UTIL "ON")
set (DMQ_THREAD "DMQ_THREAD_STDLIB")
set (DMQ_SERIALIZE "DMQ_SERIALIZE_NONE")
set (DMQ_TRANSPORT "DMQ_TRANSPORT_NONE")
include("${CMAKE_SOURCE_DIR}/DelegateMQ/DelegateMQ.cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# ------------------------------------------------------------------------------
# 2. Critical Preprocessor Definitions
# ------------------------------------------------------------------------------
add_compile_definitions(
    # DUCKDB_STATIC_BUILD  <-- REMOVED: Must NOT be defined when using DLL
    NOMINMAX               # Kills min/max macros
    WIN32_LEAN_AND_MEAN    # Excludes rare Windows headers
    NOGDI                  # Kills GetObject/GetObjectA macros
    _WINSOCKAPI_           # Prevents Winsock1 inclusion
)

# ------------------------------------------------------------------------------
# 3. Source Files
# ------------------------------------------------------------------------------
# Collect project files. Note: We explicitly EXCLUDE duckdb.cpp
file(GLOB SOURCES 
    "*.cpp" 
    "*.h" 
    "DuckDB/async_duckdb.cpp" 
    "DuckDB/async_duckdb.hpp"
    # "DuckDB/duckdb.cpp" <-- Do NOT compile this huge file anymore
)

list(APPEND SOURCES ${DMQ_PREDEF_SOURCES})

source_group("Delegate Files" FILES ${DMQ_LIB_SOURCES})
source_group("DuckDB Files" FILES "DuckDB/async_duckdb.cpp" "DuckDB/async_duckdb.hpp")

# ------------------------------------------------------------------------------
# 4. Include Paths
# ------------------------------------------------------------------------------
include_directories( 
    ${DMQ_ROOT_DIR}
    DuckDB
)

# ------------------------------------------------------------------------------
# 5. Build Target
# ------------------------------------------------------------------------------
add_executable(Async-DuckDBApp ${SOURCES} ${DMQ_LIB_SOURCES})

# ------------------------------------------------------------------------------
# 6. Linking
# ------------------------------------------------------------------------------
# Locate the duckdb.lib in the DuckDB folder
find_library(DUCKDB_LIB NAMES duckdb PATHS "${CMAKE_SOURCE_DIR}/DuckDB" NO_DEFAULT_PATH)

target_link_libraries(Async-DuckDBApp PRIVATE 
    ${DUCKDB_LIB}  # Link against duckdb.lib
    ws2_32
    rstrtmgr
    bcrypt
    winmm
)

# ------------------------------------------------------------------------------
# 7. Post-Build: Copy DLL
# ------------------------------------------------------------------------------
# Automatically copy duckdb.dll to the exe folder so the app runs
add_custom_command(TARGET Async-DuckDBApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/DuckDB/duckdb.dll"
    "$<TARGET_FILE_DIR:Async-DuckDBApp>"
)

# ------------------------------------------------------------------------------
# 8. Compiler Options
# ------------------------------------------------------------------------------
if(MSVC)
    target_compile_options(Async-DuckDBApp PRIVATE
        /MP         # Multi-processor compilation
        /wd4005     # Suppress macro redefinition warnings (still needed for Windows headers)
    )
endif()