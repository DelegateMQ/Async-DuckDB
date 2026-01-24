![License MIT](https://img.shields.io/github/license/BehaviorTree/BehaviorTree.CPP?color=blue)
[![conan Ubuntu](https://github.com/endurodave/DuckDB-SQLite/actions/workflows/cmake_ubuntu.yml/badge.svg)](https://github.com/endurodave/DuckDB-SQLite/actions/workflows/cmake_ubuntu.yml)
[![conan Ubuntu](https://github.com/endurodave/DuckDB-SQLite/actions/workflows/cmake_clang.yml/badge.svg)](https://github.com/endurodave/DuckDB-SQLite/actions/workflows/cmake_clang.yml)
[![conan Windows](https://github.com/endurodave/DuckDB-SQLite/actions/workflows/cmake_windows.yml/badge.svg)](https://github.com/endurodave/DuckDB-SQLite/actions/workflows/cmake_windows.yml)

# Asynchronous DuckDB API using C++ Delegates

An asynchronous DuckDB API wrapper implemented using a C++ delegate libraries. All target platforms are supported including Windows, Linux, and embedded systems.

# Table of Contents

- [Asynchronous DuckDB API using C++ Delegates](#asynchronous-duckdb-api-using-c-delegates)
- [Table of Contents](#table-of-contents)
- [Async-DuckDB Overview](#async-duckdb-overview)
  - [References](#references)
- [Getting Started](#getting-started)
- [Delegate Quick Start](#delegate-quick-start)

# Async-DuckDB Overview

Async-DuckDB is a thread-safe, asynchronous C++ wrapper for the DuckDB database engine, powered by the DelegateMQ messaging library.

**The Challenge**

DuckDB is a high-performance in-process SQL OLAP database designed for fast analytical queries. However, its "embedded" nature means it runs within your application's process space.

Blocking Operations: Complex analytical queries (aggregations on millions of rows) can take hundreds of milliseconds or seconds. Executing these on the main thread (GUI or request handler) freezes the application.

Thread Safety & Contention: While DuckDB supports Multi-Version Concurrency Control (MVCC), managing connection lifecycles and query execution across unrestricted threads can lead to resource contention and non-deterministic behavior in complex applications.

**The Solution**

This project leverages DelegateMQ to marshal all database operations to a dedicated background worker thread.

* **Non-Blocking API:** The main thread remains responsive. Heavy SQL operations return a std::future or trigger a callback immediately, while the work happens in the background.

* **Serialized Access:** Database tasks are serialized through a thread-safe message queue. This guarantees a deterministic order of execution without the complexity of manual mutex locking.

* **Simplified Concurrency:** Developers can write code that looks synchronous (using C++ delegates) while the execution behavior is fully asynchronous and thread-safe.

**Architecture**

All calls to the `async::Connection` proxy are automatically wrapped in a lambda and dispatched to the private DuckDB thread.

```cpp
// 1. Caller (Main Thread)
auto future = conn.QueryFuture("SELECT avg(price) FROM huge_table");

// 2. DelegateMQ (Internal)
//    - Packages the lambda
//    - Pushes to Thread Message Queue
//    - Returns std::future immediately

// 3. Worker Thread (Background)
//    - Pops message
//    - Executes DuckDB Query
//    - Sets promise value
```

This pattern ensures your application remains fluid even when crunching massive datasets.

## References

* <a href="https://github.com/endurodave/DelegateMQ">DelegatesMQ</a> - Invoke any C++ callable function synchronously, asynchronously, or on a remote endpoint.
* <a href="https://duckdb.org/">DuckDB</a> - An in-process SQL OLAP database management system designed for fast analytical queries on large datasets.

# Getting Started
[CMake](https://cmake.org/) is used to create the project build files on any Windows or Linux machine.

1. Clone the repository.
2. From the repository root, run the following CMake command:   
   `cmake -B Build .`
3. Build and run the project within the `Build` directory. 

# Delegate Quick Start

The DelegateMQ contains delegates and delegate containers. The example below creates a delegate with the target function `MyTestFunc()`. The first example is a synchronously delegate function call, and the second example asynchronously. Notice the only difference is adding a thread instance `myThread` argument. See [DelegateMQ](https://github.com/endurodave/DelegateMQ) repository for more details.

```cpp
#include "DelegateMQ.h"

using namespace DelegateMQ;

void MyTestFunc(int val)
{
    printf("%d", val);
}

int main(void)
{
    // Create a synchronous delegate
    auto syncDelegate = MakeDelegate(&MyTestFunc);

    // Invoke MyTestFunc() synchronously
    syncDelegate(123);
    // Create an asynchronous non-blocking delegate
    auto asyncDelegate = MakeDelegate(&MyTestFunc, myThread);

    // Invoke MyTestFunc() asynchronously (non-blocking)
    asyncDelegate(123);

    // Create an asynchronous blocking delegate
    auto asyncDelegateWait = MakeDelegate(&MyTestFunc, myThread, WAIT_INFINITE);

    // Invoke MyTestFunc() asynchronously (blocking)
    asyncDelegateWait(123);
}
```

