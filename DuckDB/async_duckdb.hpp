#ifndef ASYNC_DUCKDB_H
#define ASYNC_DUCKDB_H

// Asynchronous DuckDB wrapper using C++ Delegates
// @see https://github.com/endurodave/DelegateMQ
// David Lafreniere, Jan 2026

// ------------------------------------------------------------------------------------------------
// ARCHITECTURE OVERVIEW:
// ------------------------------------------------------------------------------------------------
// This library wraps the DuckDB C++ Classes to execute all database operations on a 
// dedicated background thread (Worker Thread). 
//
// PROXY PATTERN:
// Unlike the C-style SQLite wrapper, this wrapper uses Proxy Classes.
// 1. async::Database   -> Manages a duckdb::DuckDB instance on the worker thread.
// 2. async::Connection -> Manages a duckdb::Connection instance on the worker thread.
//
// LIFETIME MANAGEMENT:
// The underlying DuckDB objects are held in smart pointers. When the Proxy object goes
// out of scope on the Main Thread, it posts a "Destroy" task to the Worker Thread, ensuring
// thread-safe destruction.
// ------------------------------------------------------------------------------------------------

#include "duckdb.hpp"
#include "DelegateMQ.h" 
#include <future>
#include <memory>
#include <string>
#include <chrono>

namespace async
{
    // Default timeout for synchronous (blocking) calls
    constexpr dmq::Duration MAX_WAIT = std::chrono::minutes(2);

    // -------------------------------------------------------------------------
    // Initialization & Thread Management
    // -------------------------------------------------------------------------
    // Start the dedicated DuckDB worker thread. Must call before using any classes.
    void init_worker();

    // Stop the worker thread.
    void shutdown_worker();

    // Accessor for the internal worker thread (useful for advanced DelegateMQ usage)
    Thread* get_worker_thread();

    // -------------------------------------------------------------------------
    // Database Proxy
    // -------------------------------------------------------------------------
    // Wraps duckdb::DuckDB. Represents the physical database file (or in-memory).
    class Database {
    public:
        // Opens the database on the worker thread. Blocks until open is complete.
        // @param path: Path to DB file, or nullptr for in-memory.
        Database(const char* path, dmq::Duration timeout = MAX_WAIT);

        // Asynchronously destroys the underlying DB on the worker thread.
        ~Database();

        // Unsafe accessor: Only use if you know you are on the Worker Thread!
        duckdb::DuckDB* unsafe_raw() { return m_db.get(); }

    private:
        std::shared_ptr<duckdb::DuckDB> m_db;
    };

    // -------------------------------------------------------------------------
    // Connection Proxy
    // -------------------------------------------------------------------------
    // Wraps duckdb::Connection. Represents an active session/transaction.
    class Connection {
    public:
        // Opens a connection to the specified Database on the worker thread.
        Connection(Database& db, dmq::Duration timeout = MAX_WAIT);

        // Asynchronously closes connection on the worker thread.
        ~Connection();

        // ---------------------------------------------------------------------
        // Synchronous API (Blocking with Timeout)
        // ---------------------------------------------------------------------
        // Executes SQL and waits for the result.
        // Returns a unique_ptr to the result set. 
        // @throws std::runtime_error on timeout.
        std::unique_ptr<duckdb::QueryResult> Query(const std::string& sql, dmq::Duration timeout = MAX_WAIT);

        // ---------------------------------------------------------------------
        // Asynchronous API (Future / Non-Blocking)
        // ---------------------------------------------------------------------
        // Returns a std::future immediately. The query runs in background.
        // The future resolves when the query completes.
        std::future<std::unique_ptr<duckdb::QueryResult>> QueryFuture(const std::string& sql);

    private:
        std::unique_ptr<duckdb::Connection> m_conn;
    };

} // namespace async

#endif // ASYNC_DUCKDB_H