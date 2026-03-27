// Asynchronous DuckDB wrapper using C++ Delegates
// @see https://github.com/DelegateMQ/DelegateMQ
// David Lafreniere, Jan 2026

#include "async_duckdb.hpp"
#include "DelegateMQ.h"
#include <future>
#include <tuple>
#include <stdexcept>
#include <thread>
#include <functional> 
#include <iostream>
#include <type_traits>
#include <optional>
#include <atomic>

using namespace dmq;

namespace async
{
    // A private worker thread instance
    static Thread DuckThread("DuckDB Thread");
    static std::atomic<IThread*> CentralThread{ &DuckThread };

    const std::string PreparedStatement::m_empty_error = "";

    // --------------------------------------------------------------------------------
    // Core Logic: Dispatch to Worker Thread
    // --------------------------------------------------------------------------------
    void DispatchTask(std::function<void()> task)
    {
        IThread* thread = CentralThread.load();
        if (!thread) return;
        
        auto delegate = dmq::MakeDelegate(task, *thread);
        delegate.AsyncInvoke();
    }

    // Helper trait to check if a type is a pointer-like object to QueryResult
    template<typename T, typename = void>
    struct is_query_result_ptr : std::false_type {};

    template<typename T>
    struct is_query_result_ptr<T, std::void_t<decltype(std::declval<T>().operator->()->HasError())>> : std::true_type {};

    // Ensures a QueryResult is fully materialized on the worker thread before being
    // handed to the caller. StreamQueryResult is tied to the connection and must not
    // be accessed from another thread; this converts it to MaterializedQueryResult.
    static std::unique_ptr<duckdb::QueryResult> EnsureMaterialized(std::unique_ptr<duckdb::QueryResult> result) {
        if (!result || result->type != duckdb::QueryResultType::STREAM_RESULT) {
            return result;
        }
        return static_cast<duckdb::StreamQueryResult&>(*result).Materialize();
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
        using RetType = std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>;

        IThread* thread = CentralThread.load();
        // If we are already on the worker thread, just run it
        if (thread && thread->IsCurrentThread()) {
            return std::invoke(func, std::forward<Args>(args)...);
        }

        struct SyncState {
            dmq::Semaphore sema;
            std::exception_ptr ex;
            typename std::conditional_t<std::is_void_v<RetType>, bool, std::optional<RetType>> result;
        };
        auto state = std::make_shared<SyncState>();

        auto task = [state, func, args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            try {
                if constexpr (std::is_void_v<RetType>) {
                    std::apply(func, std::move(args));
                }
                else {
                    state->result = std::apply(func, std::move(args));
                }
            }
            catch (...) {
                state->ex = std::current_exception();
            }
            state->sema.Signal();
            };

        DispatchTask(task);

        if (!state->sema.Wait(timeout)) {
            throw std::runtime_error("DuckDB Operation Timed Out");
        }

        if (state->ex) {
            std::rethrow_exception(state->ex);
        }
        
        if constexpr (!std::is_void_v<RetType>) {
            auto res = std::move(*(state->result));
            
            // Check if RetType is some kind of smart pointer to QueryResult
            if constexpr (is_query_result_ptr<RetType>::value) {
                if (res && res->HasError()) {
                    res->ThrowError();
                }
            }
            return res;
        }
    }

    // --------------------------------------------------------------------------------
    // Initialization & Thread Management
    // --------------------------------------------------------------------------------
    void init_worker(IThread* thread) { 
        if (thread) {
            CentralThread.store(thread);
        } else {
            DuckThread.CreateThread(); 
            CentralThread.store(&DuckThread);
        }
    }
    
    void shutdown_worker() { 
        IThread* thread = CentralThread.load();
        if (thread == &DuckThread) {
            DuckThread.ExitThread(); 
        }
        CentralThread.store(nullptr);
    }
    
    IThread* get_worker_thread() { return CentralThread.load(); }

    // --------------------------------------------------------------------------------
    // Database Proxy
    // --------------------------------------------------------------------------------
    Database::Database(const char* path, dmq::Duration timeout) {
        auto task = [p = (path ? std::string(path) : std::string())]() -> std::shared_ptr<duckdb::DuckDB> {
            const char* dbPath = p.empty() ? nullptr : p.c_str();
            return std::make_shared<duckdb::DuckDB>(dbPath);
        };
        m_db = RunSync(timeout, task);
    }

    Database::~Database() {
        if (!m_db) return;
        auto task = [db = std::move(m_db)]() {
            // db shared_ptr destroyed here on worker thread
        };
        DispatchTask(task);
    }

    // --------------------------------------------------------------------------------
    // PreparedStatement Proxy
    // --------------------------------------------------------------------------------
    PreparedStatement::~PreparedStatement() {
        if (m_state) {
            auto task = [state = std::move(m_state)]() {
                // state shared_ptr destroyed here on worker thread
            };
            DispatchTask(task);
        }
    }

    void PreparedStatement::BindValue(duckdb::idx_t index, duckdb::Value val) {
        if (index == 0) throw std::out_of_range("DuckDB Bind index starts at 1");
        if (index > 1000) throw std::out_of_range("DuckDB Bind index exceeds sanity limit");

        auto task = [state = m_state, index, v = std::move(val)]() {
            if (state->stmt && state->stmt->success) {
                if (state->params.size() < index) {
                    state->params.resize(index);
                }
                state->params[index - 1] = v;
            }
            };
        RunSync(MAX_WAIT, task);
    }

    std::unique_ptr<duckdb::QueryResult> PreparedStatement::Execute(dmq::Duration timeout) {
        auto task = [state = m_state]() -> std::unique_ptr<duckdb::QueryResult> {
            return EnsureMaterialized(state->stmt->Execute(state->params));
        };
        return RunSync(timeout, task);
    }

    std::future<std::unique_ptr<duckdb::QueryResult>> PreparedStatement::ExecuteFuture() {
        auto task = [state = m_state]() -> std::unique_ptr<duckdb::QueryResult> {
            return EnsureMaterialized(state->stmt->Execute(state->params));
            };
        return RunAsync(task);
    }

    duckdb::idx_t PreparedStatement::nParam() {
        return RunSync(MAX_WAIT, [state = m_state]() {
            if (!state->stmt || !state->stmt->success) return (duckdb::idx_t)0;
            return (duckdb::idx_t)state->stmt->GetExpectedParameterTypes().size();
            });
    }

    // --------------------------------------------------------------------------------
    // Appender Proxy
    // --------------------------------------------------------------------------------
    Appender::~Appender() {
        if (m_appender) {
            auto task = [app = std::move(m_appender)]() {
                try {
                    app->Flush();
                    app->Close();
                } catch (...) {}
                };
            DispatchTask(task);
        }
    }

    void Appender::BeginRow() {
        if (!m_appender) return;
        m_pending_row.clear();
    }

    void Appender::EndRow() {
        if (!m_appender) return;
        RunSync(MAX_WAIT, [app = m_appender, row = std::move(m_pending_row)]() {
            app->BeginRow();
            for (const auto& v : row) {
                app->Append(v);
            }
            app->EndRow();
        });
    }

    void Appender::Append(const char* val) {
        AppendValue(duckdb::Value(val));
    }

    void Appender::AppendValue(duckdb::Value val) {
        m_pending_row.push_back(std::move(val));
    }

    void Appender::Flush() {
        if (!m_appender) return;
        RunSync(MAX_WAIT, [app = m_appender]() { app->Flush(); });
    }

    void Appender::Close() {
        if (!m_appender) return;
        RunSync(MAX_WAIT, [app = m_appender]() { app->Close(); });
    }

    // --------------------------------------------------------------------------------
    // Connection Proxy
    // --------------------------------------------------------------------------------
    Connection::Connection(Database& db, dmq::Duration timeout) {
        auto db_shared = db.get_internal();
        auto task = [db_shared]() -> std::shared_ptr<duckdb::Connection> {
            return std::make_shared<duckdb::Connection>(*db_shared);
        };
        m_conn = RunSync(timeout, task);
    }

    Connection::~Connection() {
        if (!m_conn) return;
        auto task = [conn = std::move(m_conn)]() {
            // conn shared_ptr destroyed here on worker thread
        };
        DispatchTask(task);
    }

    std::unique_ptr<duckdb::QueryResult> Connection::Query(const std::string& sql, dmq::Duration timeout) {
        auto task = [conn = m_conn, sql]() -> std::unique_ptr<duckdb::QueryResult> {
            return EnsureMaterialized(conn->Query(sql));
        };
        return RunSync(timeout, task);
    }

    std::future<std::unique_ptr<duckdb::QueryResult>> Connection::QueryFuture(const std::string& sql) {
        auto task = [conn = m_conn, sql]() -> std::unique_ptr<duckdb::QueryResult> {
            return EnsureMaterialized(conn->Query(sql));
            };
        return RunAsync(task);
    }

    std::unique_ptr<PreparedStatement> Connection::Prepare(const std::string& sql, dmq::Duration timeout) {
        auto task = [conn = m_conn, sql]() -> std::unique_ptr<PreparedStatement> {
            auto raw_stmt = conn->Prepare(sql);
            if (!raw_stmt->success) {
                raw_stmt->error.Throw();
            }
            std::shared_ptr<duckdb::PreparedStatement> shared_stmt = std::move(raw_stmt);
            return std::make_unique<PreparedStatement>(shared_stmt);
            };
        return RunSync(timeout, task);
    }

    std::unique_ptr<Appender> Connection::CreateAppender(const std::string& table, dmq::Duration timeout) {
        auto task = [conn = m_conn, table]() -> std::unique_ptr<Appender> {
            auto raw_app = std::make_unique<duckdb::Appender>(*conn, table);
            std::shared_ptr<duckdb::Appender> shared_app = std::move(raw_app);
            return std::make_unique<Appender>(shared_app);
            };
        return RunSync(timeout, task);
    }

    void Connection::BeginTransaction() {
        RunSync(MAX_WAIT, [conn = m_conn]() { conn->BeginTransaction(); });
    }

    void Connection::Commit() {
        RunSync(MAX_WAIT, [conn = m_conn]() { conn->Commit(); });
    }

    void Connection::Rollback() {
        RunSync(MAX_WAIT, [conn = m_conn]() { conn->Rollback(); });
    }

} // namespace async
