# Async-DuckDB

[![License MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Build Ubuntu](https://github.com/DelegateMQ/Async-DuckDB/actions/workflows/cmake_ubuntu.yml/badge.svg)](https://github.com/DelegateMQ/Async-DuckDB/actions/workflows/cmake_ubuntu.yml)
[![Build Clang](https://github.com/DelegateMQ/Async-DuckDB/actions/workflows/cmake_clang.yml/badge.svg)](https://github.com/DelegateMQ/Async-DuckDB/actions/workflows/cmake_clang.yml)
[![Build Windows](https://github.com/DelegateMQ/Async-DuckDB/actions/workflows/cmake_windows.yml/badge.svg)](https://github.com/DelegateMQ/Async-DuckDB/actions/workflows/cmake_windows.yml)

An asynchronous, thread-safe C++ wrapper for [DuckDB](https://duckdb.org/) using [DelegateMQ](https://github.com/DelegateMQ/DelegateMQ) for task-based marshalling.

---

## Table of Contents
- [Async-DuckDB](#async-duckdb)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
  - [Architectural Benefits](#architectural-benefits)
  - [References](#references)
  - [Quick Start](#quick-start)
  - [Design Pattern](#design-pattern)
  - [API Usage](#api-usage)
    - [Simple Query](#simple-query)
    - [Asynchronous Futures](#asynchronous-futures)
    - [Prepared Statements](#prepared-statements)
    - [Appender (Bulk Loading)](#appender-bulk-loading)
    - [Transactions](#transactions)
  - [Stress Testing](#stress-testing)
  - [Performance: Serialized Entry vs. Parallel Execution](#performance-serialized-entry-vs-parallel-execution)
  - [DuckDB Library Setup](#duckdb-library-setup)
    - [Local Development Setup](#local-development-setup)
    - [Continuous Integration (CI)](#continuous-integration-ci)
  - [Build Instructions](#build-instructions)
    - [Prerequisites](#prerequisites)
    - [Windows (MSVC)](#windows-msvc)

---

## Overview

DuckDB is a high-performance analytical database designed to run in-process. While it is incredibly fast for OLAP tasks, its internal architecture means that **Connection** and **PreparedStatement** objects are **not thread-safe**. Sharing a single connection across multiple threads without manual synchronization leads to undefined behavior or crashes.

**Async-DuckDB** provides a non-blocking, thread-safe proxy layer. It allows any number of application threads to share a single DuckDB database instance by marshalling all calls to a dedicated worker thread. This eliminates the need for complex locking in your application code.

## Architectural Benefits

*   **Thread Affinity**: All raw DuckDB engine calls are executed on a single background "DuckDB Thread."
*   **Eliminates Lock Contention**: User threads never block each other waiting for database access; tasks are queued and executed sequentially.
*   **Non-Blocking UI/Logic**: Heavy analytical queries can be dispatched using `std::future`, keeping the main thread responsive.
*   **Materialized Results**: Query results are automatically materialized on the worker thread before being returned, ensuring they are safe to access from any thread.
*   **Sequential Integrity**: Database operations are guaranteed to execute in the order they were dispatched (FIFO).

## References

- [DuckDB](https://duckdb.org/) - The analytical database engine.
- [DelegateMQ](https://github.com/DelegateMQ/DelegateMQ) - The underlying messaging middleware used for thread marshalling.

## Quick Start

```cpp
#include "DuckDB/async_duckdb.hpp"

int main() {
    async::init_worker(); // Start the background thread

    async::Database db(":memory:");
    async::Connection conn(db);

    conn.Query("CREATE TABLE test (val INTEGER);");
    conn.Query("INSERT INTO test VALUES (42);");

    auto result = conn.Query("SELECT * FROM test;");
    // Use result...

    async::shutdown_worker();
}
```

## Design Pattern

Async-DuckDB employs the **Proxy/Worker** pattern using C++ Delegates:
1.  **Proxy Objects**: `async::Connection`, `async::PreparedStatement`, etc., act as lightweight handles.
2.  **Delegate Marshalling**: When an API is called, a delegate is created containing the task.
3.  **Task Queue**: The delegate is dispatched to the background worker thread's message queue.
4.  **Synchronous/Asynchronous Execution**: The proxy waits for a signal (Sync) or returns a `std::future` (Async).

---

## API Usage

### Simple Query
Standard blocking call. The calling thread waits until the query completes on the worker thread.
```cpp
auto result = conn.Query("SELECT name FROM users WHERE id = 1;");
```

### Asynchronous Futures
Dispatches the query and returns immediately. Ideal for long-running analytical tasks.
```cpp
std::future<std::unique_ptr<duckdb::QueryResult>> future = conn.QueryFuture("SELECT avg(val) FROM large_table;");

// ... do other work ...

auto result = future.get(); // Get the result when ready
```

### Prepared Statements
Thread-safe parameter binding. Each `Bind` call is marshalled to the worker thread to ensure the statement state is updated correctly before execution.
```cpp
auto stmt = conn.Prepare("INSERT INTO products VALUES (?, ?, ?);");
stmt->Bind(1, 101);
stmt->Bind(2, "Laptop");
stmt->Bind(3, 1200.50);
stmt->Execute();
```

### Appender (Bulk Loading)
Wraps the DuckDB Appender API for high-speed data ingestion.
```cpp
auto appender = conn.CreateAppender("metrics");
appender->BeginRow();
appender->Append(1.5);
appender->Append("cpu_usage");
appender->EndRow();
appender->Flush();
```

### Transactions
Full support for ACID transactions.
```cpp
conn.BeginTransaction();
try {
    conn.Query("UPDATE accounts SET balance = balance - 100 WHERE id = 1;");
    conn.Commit();
} catch (...) {
    conn.Rollback();
}
```

---

## Stress Testing

The repository includes a rigorous stress test (`Examples/StressTest.cpp`) that:
- Spawns **8 concurrent threads**.
- Performs **1,600 mixed operations** (synchronous inserts and asynchronous queries).
- Validates data integrity and ensures zero race conditions in parameter binding.

## Performance: Serialized Entry vs. Parallel Execution

A common question is whether marshalling all calls to a single worker thread limits DuckDB's famous multi-threaded performance. The answer is **no**.

*   **Serialized Entry**: `Async-DuckDB` serializes the *entry point* (the API calls) to ensure that the `Connection` and `PreparedStatement` state is never corrupted by concurrent access. This prevents "Database is locked" errors and race conditions.
*   **Parallel Execution**: Once a query is dispatched to the DuckDB engine, its internal **Morsel-Driven Parallelism** kicks in. DuckDB will still automatically spawn its own internal tasks across all available CPU cores to execute that single query as fast as possible.

**The result**: You get the absolute thread-safety of a serialized proxy with the full analytical muscle of DuckDB's parallel columnar engine.

## DuckDB Library Setup

The project requires the DuckDB C++ library (amalgamation) and binaries. 

### Local Development Setup
For local development, you must place the DuckDB library files in the `DuckDB/` directory:

1.  **Download**: Visit the [DuckDB Releases](https://github.com/duckdb/duckdb/releases/tag/v1.4.3) page.
2.  **Windows**: 
    - Download `libduckdb-windows-amd64.zip`.
    - Extract `duckdb.lib` and `duckdb.dll` into the `DuckDB/` folder.
3.  **Linux/macOS**:
    - Download the appropriate `libduckdb` zip for your architecture.
    - Extract `libduckdb.so` (or `.a`) into the `DuckDB/` folder.
4.  **Headers**: Ensure `duckdb.h` and `duckdb.hpp` are also in the `DuckDB/` folder (these are typically included in the amalgamation or lib zip).

The `CMakeLists.txt` is configured to look in the `DuckDB/` directory first (`NO_DEFAULT_PATH`) to ensure the correct version is linked.

### Continuous Integration (CI)
The included GitHub Action workflows (`.github/workflows/`) automatically download the correct DuckDB v1.4.3 binaries for Windows and Ubuntu during the build process. No manual setup is required for CI.

---

## Build Instructions

### Prerequisites
- C++17 compatible compiler (MSVC, GCC, or Clang).
- CMake 3.10+.

### Windows (MSVC)
```powershell
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

The build produces a single executable `Async-DuckDBApp` which runs all examples, stress tests, and unit tests.

---
*Created by David Lafreniere, 2026.*
