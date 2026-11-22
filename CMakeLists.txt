# Example CMake command line to create project build files:
#
# cmake -B build .
#
# DelegateMQ is fetched from GitHub at the release tag DMQ_GIT_TAG when CMake
# configures. To build against a local DelegateMQ checkout instead, add
# -DFETCHCONTENT_SOURCE_DIR_DELEGATEMQ=<path>. To configure again without
# network access, add -DFETCHCONTENT_FULLY_DISCONNECTED=ON.

# Specify the minimum CMake version required
cmake_minimum_required(VERSION 3.16)

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

# DelegateMQ release to build against
set(DMQ_GIT_TAG "v3.0.0" CACHE STRING "DelegateMQ release tag (or branch) to fetch")

include(FetchContent)
FetchContent_Declare(
    delegatemq
    GIT_REPOSITORY https://github.com/DelegateMQ/DelegateMQ.git
    GIT_TAG        ${DMQ_GIT_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(delegatemq)

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
    "Examples/SimpleExample.cpp"
    "Examples/FutureExample.cpp"
    "Examples/PreparedStatementExample.cpp"
    "Examples/AppenderExample.cpp"
    "Examples/TransactionExample.cpp"
    "Examples/StressTest.cpp"
)

source_group("DuckDB Files" FILES "DuckDB/async_duckdb.cpp" "DuckDB/async_duckdb.hpp")
source_group("Example Files" FILES 
    "Examples/SimpleExample.cpp" 
    "Examples/FutureExample.cpp" 
    "Examples/PreparedStatementExample.cpp" 
    "Examples/AppenderExample.cpp" 
    "Examples/TransactionExample.cpp"
    "Examples/StressTest.cpp"
)

# ------------------------------------------------------------------------------
# 4. Include Paths
# ------------------------------------------------------------------------------
include_directories( 
    DuckDB
    UnitTest
    Examples
)

# ------------------------------------------------------------------------------
# 5. Build Target
# ------------------------------------------------------------------------------
add_executable(Async-DuckDBApp ${SOURCES})

# Add subdirectories to build
add_subdirectory(UnitTest)


# ------------------------------------------------------------------------------
# 6. Linking
# ------------------------------------------------------------------------------
# Locate the duckdb library
find_library(DUCKDB_LIB NAMES duckdb libduckdb PATHS "${CMAKE_SOURCE_DIR}/DuckDB" NO_DEFAULT_PATH)

target_link_libraries(Async-DuckDBApp PRIVATE 
    ${DUCKDB_LIB}  # Link against duckdb library
    $<$<PLATFORM_ID:Windows>:ws2_32 rstrtmgr bcrypt winmm>
    UnitTestLib
    DelegateMQ::DelegateMQ
)

# ------------------------------------------------------------------------------
# 7. Post-Build: Copy DLL (Windows Only)
# ------------------------------------------------------------------------------
# Automatically copy duckdb.dll to the exe folder so the app runs on Windows
if (WIN32)
    add_custom_command(TARGET Async-DuckDBApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/DuckDB/duckdb.dll"
        "$<TARGET_FILE_DIR:Async-DuckDBApp>"
    )
endif()

# ------------------------------------------------------------------------------
# 8. Compiler Options
# ------------------------------------------------------------------------------
if(MSVC)
    target_compile_options(Async-DuckDBApp PRIVATE
        /MP         # Multi-processor compilation
        /wd4005     # Suppress macro redefinition warnings
        /wd4146     # Suppress 'unary minus applied to unsigned' (DuckDB header warning)
    )
endif()