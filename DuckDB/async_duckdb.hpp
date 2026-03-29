// async_duckdb.hpp
// Asynchronous C++ wrapper for DuckDB.
// 
// Provides a thread-safe, non-blocking interface to the DuckDB engine using the 
// Proxy-Worker architectural pattern. This wrapper ensures that all raw DuckDB 
// operations (which are not natively thread-safe) are marshalled to a dedicated 
// background worker thread.
// 
// Key Features:
// - Thread-Safe Proxies: Connection, PreparedStatement, and Appender objects can 
//   be used safely from any application thread.
// - Non-Blocking APIs: Supports QueryFuture for asynchronous execution.
// - Materialized Results: Ensures query data is fully fetched on the worker 
//   thread before being handed back to the user thread.
// - Automated Marshalling: Leverages DelegateMQ for task dispatching.
// 
// @author David Lafreniere
// @date 2026
// @see https://github.com/DelegateMQ/Async-DuckDB

#ifndef ASYNC_DUCKDB_H
#define ASYNC_DUCKDB_H

#include "duckdb.hpp"
#include "DelegateMQ.h" 
#include <future>
#include <memory>
#include <string>
#include <chrono>
#include <vector>
#include <functional>
#include <stdexcept>

namespace async
{
    // Default timeout for synchronous (blocking) calls
    constexpr dmq::Duration MAX_WAIT = std::chrono::minutes(2);

    // -------------------------------------------------------------------------
    // Initialization & Thread Management
    // -------------------------------------------------------------------------
    void init_worker(dmq::IThread* thread = nullptr);
    void shutdown_worker();
    dmq::IThread* get_worker_thread();

    // Internal helper to marshal tasks to the worker thread
    void DispatchTask(std::function<void()> task);

    // -------------------------------------------------------------------------
    // Database Proxy
    // -------------------------------------------------------------------------
    class Database {
    public:
        Database(const char* path, dmq::Duration timeout = MAX_WAIT);
        ~Database();

        std::shared_ptr<duckdb::DuckDB> get_internal() { return m_db; }

    private:
        std::shared_ptr<duckdb::DuckDB> m_db;
    };

    // -------------------------------------------------------------------------
    // PreparedStatement Proxy
    // -------------------------------------------------------------------------
    class PreparedStatement {
    public:
        struct State {
            std::shared_ptr<duckdb::PreparedStatement> stmt;
            duckdb::vector<duckdb::Value> params;
        };

        explicit PreparedStatement(std::shared_ptr<duckdb::PreparedStatement> stmt)
            : m_state(std::make_shared<State>()) {
            m_state->stmt = std::move(stmt);
        }

        ~PreparedStatement();

        // ---------------------------------------------------------------------
        // Binding API
        // ---------------------------------------------------------------------
        template <typename T>
        void Bind(duckdb::idx_t index, T val) {
            BindValue(index, duckdb::Value(val));
        }

        std::unique_ptr<duckdb::QueryResult> Execute(dmq::Duration timeout = MAX_WAIT);
        std::future<std::unique_ptr<duckdb::QueryResult>> ExecuteFuture();
        duckdb::idx_t nParam();
        bool Success() const { return m_state->stmt && m_state->stmt->success; }
        const std::string& GetError() const { return m_state->stmt ? m_state->stmt->GetError() : m_empty_error; }

    private:
        void BindValue(duckdb::idx_t index, duckdb::Value val);

        std::shared_ptr<State> m_state;
        static const std::string m_empty_error;
    };

    // -------------------------------------------------------------------------
    // Appender Proxy
    // -------------------------------------------------------------------------
    class Appender {
    public:
        explicit Appender(std::shared_ptr<duckdb::Appender> appender)
            : m_appender(std::move(appender)) {
        }

        ~Appender();

        void BeginRow();
        void EndRow();

        template <typename T>
        void Append(T val) {
            AppendValue(duckdb::Value(val));
        }

        void Append(const char* val);

        void Flush();
        void Close();

    private:
        void AppendValue(duckdb::Value val);

        std::shared_ptr<duckdb::Appender> m_appender;
        std::vector<duckdb::Value>        m_pending_row;
    };

    // -------------------------------------------------------------------------
    // Connection Proxy
    // -------------------------------------------------------------------------
    class Connection {
    public:
        Connection(Database& db, dmq::Duration timeout = MAX_WAIT);
        ~Connection();

        std::unique_ptr<duckdb::QueryResult> Query(const std::string& sql, dmq::Duration timeout = MAX_WAIT);
        std::future<std::unique_ptr<duckdb::QueryResult>> QueryFuture(const std::string& sql);

        std::unique_ptr<PreparedStatement> Prepare(const std::string& sql, dmq::Duration timeout = MAX_WAIT);
        std::unique_ptr<Appender> CreateAppender(const std::string& table, dmq::Duration timeout = MAX_WAIT);

        void BeginTransaction();
        void Commit();
        void Rollback();

    private:
        std::shared_ptr<duckdb::Connection> m_conn;
    };

} // namespace async

#endif // ASYNC_DUCKDB_H
