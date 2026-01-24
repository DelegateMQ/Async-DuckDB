// Asynchronous DuckDB wrapper using C++ Delegates
// @see https://github.com/endurodave/DelegateMQ
// David Lafreniere, Jan 2026

#include "async_duckdb.hpp"
#include "DelegateMQ.h"
#include <future>
#include <tuple>
#include <stdexcept>
#include <thread>
#include <functional> 
#include <iostream>

using namespace dmq;

namespace async
{
    // A private worker thread instance
    static Thread DuckThread("DuckDB Thread");

    // --------------------------------------------------------------------------------
    // Core Logic: Dispatch to Worker Thread
    // --------------------------------------------------------------------------------
    void DispatchTask(std::function<void()> task)
    {
        if (DuckThread.GetThreadId() == std::thread::id()) {
            // If we are shutting down or not initialized, we can't dispatch.
            // In a destructor, throwing is bad, so we log or ignore.
            // For now, we assume init_worker() was called.
            return;
        }

        auto delegate = dmq::MakeDelegate(task, DuckThread);
        delegate.AsyncInvoke();
    }

    // --------------------------------------------------------------------------------
    // Helper: RunAsync (Returns std::future immediately)
    // --------------------------------------------------------------------------------
    template <typename Func, typename... Args>
    auto RunAsync(Func func, Args&&... args)
    {
        using RetType = std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>;

        auto promise = std::make_shared<std::promise<RetType>>();
        auto future = promise->get_future();

        // Lambda captures 'func' and 'args' by value (COPY). 
        // Func must be copy-constructible to satisfy std::function requirements.
        auto task = [promise, func, args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            try {
                if constexpr (std::is_void_v<RetType>) {
                    std::apply(func, std::move(args));
                    promise->set_value();
                }
                else {
                    promise->set_value(std::apply(func, std::move(args)));
                }
            }
            catch (...) {
                promise->set_exception(std::current_exception());
            }
            };

        DispatchTask(task);
        return future;
    }

    // --------------------------------------------------------------------------------
    // Helper: RunSync (Blocks until done, returns value)
    // --------------------------------------------------------------------------------
    template <typename Func, typename... Args>
    auto RunSync(dmq::Duration timeout, Func func, Args&&... args)
    {
        using RetType = decltype(func(std::forward<Args>(args)...));

        // Optimization: If already on worker thread, run directly
        if (DuckThread.GetThreadId() == std::this_thread::get_id()) {
            return std::invoke(func, std::forward<Args>(args)...);
        }

        auto promise = std::make_shared<std::promise<RetType>>();
        auto future = promise->get_future();

        auto task = [promise, func, args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            try {
                if constexpr (std::is_void_v<RetType>) {
                    std::apply(func, std::move(args));
                    promise->set_value();
                }
                else {
                    promise->set_value(std::apply(func, std::move(args)));
                }
            }
            catch (...) {
                promise->set_exception(std::current_exception());
            }
            };

        DispatchTask(task);

        if (future.wait_for(timeout) == std::future_status::timeout) {
            throw std::runtime_error("DuckDB Operation Timed Out");
        }
        return future.get();
    }

    // --------------------------------------------------------------------------------
    // Implementation
    // --------------------------------------------------------------------------------
    void init_worker() { DuckThread.CreateThread(); }
    void shutdown_worker() { DuckThread.ExitThread(); }
    Thread* get_worker_thread() { return &DuckThread; }

    // --- Database ---
    Database::Database(const char* path, dmq::Duration timeout) {
        auto task = [this, p = (path ? std::string(path) : std::string())]() {
            const char* dbPath = p.empty() ? nullptr : p.c_str();
            m_db = std::make_shared<duckdb::DuckDB>(dbPath);
            };
        RunSync(timeout, task);
    }

    Database::~Database() {
        if (!m_db) return;
        // m_db is shared_ptr, so capturing it by value is copyable. Safe for std::function.
        auto task = [db = m_db]() { /* db destroyed here on worker */ };
        DispatchTask(task);
    }

    // --- Connection ---
    Connection::Connection(Database& db, dmq::Duration timeout) {
        auto task = [this, &db]() {
            m_conn = std::make_unique<duckdb::Connection>(*db.unsafe_raw());
            };
        RunSync(timeout, task);
    }

    Connection::~Connection() {
        if (!m_conn) return;

        // [FIX] Convert unique_ptr to shared_ptr so the lambda is Copy Constructible.
        // std::function requires the lambda to be copyable.
        // std::unique_ptr is move-only, which breaks std::function.
        std::shared_ptr<duckdb::Connection> shared_conn = std::move(m_conn);

        auto task = [shared_conn]() {
            // shared_conn goes out of scope here, destroying the connection on the worker thread.
            };

        DispatchTask(task);
    }

    // Sync Query
    std::unique_ptr<duckdb::QueryResult> Connection::Query(const std::string& sql, dmq::Duration timeout) {
        auto task = [this, sql]() { return m_conn->Query(sql); };
        return RunSync(timeout, task);
    }

    // Async Future Query
    std::future<std::unique_ptr<duckdb::QueryResult>> Connection::QueryFuture(const std::string& sql) {
        // Explicit return type forces upcast
        auto task = [this, sql]() -> std::unique_ptr<duckdb::QueryResult> {
            return m_conn->Query(sql);
            };
        return RunAsync(task);
    }

} // namespace async