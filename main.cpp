// Asynchronous DuckDB API's implemented using C++ delegates
// @see https://github.com/endurodave/Async-DuckDB
// @see https://github.com/endurodave/DelegateMQ
// David Lafreniere, 2026.

#include "DelegateMQ.h"
#include "async_duckdb.hpp"
#include <stdio.h>
#include <string>
#include <iostream>
#include <vector>
#include <atomic>
#include <cstdarg>

using namespace std;
using namespace dmq;

// -----------------------------------------------------------------------------
// Globals & Utils
// -----------------------------------------------------------------------------

// Worker thread instances
Thread workerThreads[] = {
    { "WorkerThread1" },
    { "WorkerThread2" }
};

Thread nonBlockingAsyncThread("NonBlockingAsyncThread");

static const int WORKER_THREAD_CNT = sizeof(workerThreads) / sizeof(workerThreads[0]);

static std::mutex mtx;
static std::condition_variable cv;
static bool ready = false;
static std::mutex printMutex;

// Global DuckDB objects for the multithreaded example
static std::unique_ptr<async::Database> db_multithread;
static std::unique_ptr<async::Connection> conn_multithread;

static MulticastDelegateSafe<void(int)> completeCallback;
static std::atomic<bool> completeFlag = false;

std::atomic<bool> processTimerExit = false;
static void ProcessTimers()
{
    while (!processTimerExit.load())
    {
        Timer::ProcessTimers();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

// Thread safe printf
void printf_safe(const char* format, ...)
{
    std::lock_guard<std::mutex> lock(printMutex);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

// Helper to print DuckDB Results
void PrintResult(duckdb::QueryResult* result)
{
    if (!result || result->HasError()) {
        printf_safe("Query Error: %s\n", result ? result->GetError().c_str() : "Unknown");
        return;
    }

    // Print headers
    std::string headerLine;
    for (const auto& name : result->names) headerLine += name + "\t";
    printf_safe("%s\n", headerLine.c_str());

    // Print rows
    for (auto& row : *result) {
        std::string rowLine;
        for (duckdb::idx_t i = 0; i < result->ColumnCount(); i++) {
            // [FIX] Explicitly request <duckdb::Value> from the template method
            rowLine += row.GetValue<duckdb::Value>(i).ToString() + "\t";
        }
        printf_safe("%s\n", rowLine.c_str());
    }
}

// -----------------------------------------------------------------------------
// Example 1: Simple Asynchronous Execution
// -----------------------------------------------------------------------------
void example1() // [FIX] Renamed to match main() call
{
    printf_safe("\n--- Example 1: Simple ---\n");

    // 1. Open Database (Creates 'async_simple.db' on disk)
    async::Database db("async_simple.db");
    async::Connection conn(db);

    // 2. Create Table
    conn.Query("CREATE TABLE IF NOT EXISTS people (id INTEGER, name VARCHAR);");
    printf_safe("Table created.\n");

    // 3. Insert
    conn.Query("INSERT INTO people VALUES (1, 'John'), (2, 'Jane');");
    printf_safe("Records inserted.\n");

    // 4. Select
    auto result = conn.Query("SELECT * FROM people;");
    PrintResult(result.get());
}

// -----------------------------------------------------------------------------
// Example 3: Multithreaded Blocking
// -----------------------------------------------------------------------------
// Simulates multiple threads pounding the single DB connection.
int async_multithread_example()
{
    printf_safe("\n--- Example 3: Multithreaded Write ---\n");

    // Initialize the global DB for this example
    db_multithread = std::make_unique<async::Database>("async_multi.db");
    conn_multithread = std::make_unique<async::Connection>(*db_multithread);

    conn_multithread->Query("CREATE TABLE IF NOT EXISTS threads (tname VARCHAR, cnt INTEGER);");

    // Lambda to write data (executed by worker threads)
    auto WriteDatabaseLambda = +[](std::string thread_name) -> void
        {
            static int cnt = 0;

            // Perform 10 Inserts
            for (int i = 0; i < 10; i++)
            {
                std::string sql = "INSERT INTO threads VALUES ('" + thread_name + "', " + std::to_string(i) + ");";

                // This blocks THIS worker thread, but the DB execution happens on the DB thread.
                // The DelegateMQ serialization ensures thread safety.
                conn_multithread->Query(sql);
            }
            printf_safe("[%s] Finished inserting.\n", thread_name.c_str());

            // Check if all threads are done
            if (++cnt >= WORKER_THREAD_CNT)
            {
                std::lock_guard<std::mutex> lock(mtx);
                ready = true;
                cv.notify_all();
            }
        };

    // Launch workers
    ready = false;
    for (int i = 0; i < WORKER_THREAD_CNT; i++)
    {
        auto delegate = MakeDelegate(WriteDatabaseLambda, workerThreads[i]);
        delegate(workerThreads[i].GetThreadName());
    }

    // Wait for workers
    std::unique_lock<std::mutex> lock(mtx);
    while (!ready) cv.wait(lock);

    // Verify Results
    auto result = conn_multithread->Query("SELECT tname, COUNT(*) FROM threads GROUP BY tname;");
    PrintResult(result.get());

    // Cleanup globals
    conn_multithread.reset();
    db_multithread.reset();

    // Notify completion callback
    completeCallback(0);
    return 0;
}

// -----------------------------------------------------------------------------
// Example 2: Internal Thread Scheduling
// -----------------------------------------------------------------------------
void example2()
{
    printf_safe("\n--- Example 2: Internal Thread Scheduling ---\n");

    // Get the internal DuckDB thread
    Thread* sqlThread = async::get_worker_thread();

    // Run Example 1 entirely on the DB thread
    // Note: We cast example1 to void(*)() to match delegate signature if needed
    auto delegate = MakeDelegate(&example1, *sqlThread, async::MAX_WAIT);
    delegate.AsyncInvoke();
}

// -----------------------------------------------------------------------------
// Example 3 Wrapper (Timing)
// -----------------------------------------------------------------------------
std::chrono::microseconds example3()
{
    auto start = std::chrono::high_resolution_clock::now();
    async_multithread_example();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start);
}

// -----------------------------------------------------------------------------
// Example 4: Multithreaded Non-Blocking (Async invoke of Example 3)
// -----------------------------------------------------------------------------
std::chrono::microseconds example4()
{
    printf_safe("\n--- Example 4: Non-Blocking Execution ---\n");
    completeFlag = false;

    // Register callback
    auto CompleteCallbackLambda = +[](int retVal) { completeFlag = true; };
    completeCallback += MakeDelegate(CompleteCallbackLambda);

    auto start = std::chrono::high_resolution_clock::now();

    // Run Example 3 on a background thread (fire and forget)
    auto noWaitDelegate = MakeDelegate(&async_multithread_example, nonBlockingAsyncThread);
    noWaitDelegate.AsyncInvoke();

    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start);
}

// -----------------------------------------------------------------------------
// Example 5: Future/Async API
// -----------------------------------------------------------------------------
void example_future()
{
    printf_safe("\n--- Example 5: Future/Async ---\n");

    async::Database db(nullptr); // In-memory DB
    async::Connection conn(db);

    conn.Query("CREATE TABLE heavy_data (id INTEGER, val VARCHAR);");

    printf_safe("[Main] Launching async insert...\n");

    // 1. Get Future
    auto future = conn.QueryFuture("INSERT INTO heavy_data VALUES (42, 'Deep Thought');");

    // 2. Do work while query runs
    printf_safe("[Main] Doing other work...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 3. Wait for result
    auto result = future.get(); // Blocks here if not ready

    if (!result->HasError()) {
        // Safe cast for RowCount check
        if (result->type == duckdb::QueryResultType::MATERIALIZED_RESULT) {
            auto mat_res = (duckdb::MaterializedQueryResult*)result.get();
            printf_safe("[Main] Success! Rows affected: %lld\n", mat_res->RowCount());
        }
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main(void)
{
    // 1. Cleanup old files
    std::remove("async_simple.db");
    std::remove("async_multi.db");

    // 2. Start Timer Thread
    std::thread timerThread(ProcessTimers);

    // 3. Start Workers
    nonBlockingAsyncThread.CreateThread();
    for (int i = 0; i < WORKER_THREAD_CNT; i++)
        workerThreads[i].CreateThread();

    // 4. Initialize DuckDB Worker
    async::init_worker();

    // 5. Run Examples
    example1(); // Simple
    example2(); // On DB Thread
    auto tBlocking = example3(); // Multithreaded Blocking
    auto tAsync = example4();    // Multithreaded Async
    example_future(); // Future

    // Wait for Example 4 to finish
    while (!completeFlag) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 6. Shutdown
    nonBlockingAsyncThread.ExitThread();
    for (int i = 0; i < WORKER_THREAD_CNT; i++)
        workerThreads[i].ExitThread();

    async::shutdown_worker();

    // Stats
    std::cout << "\n------------------------------------------------\n";
    std::cout << "Blocking Time: " << tBlocking.count() << " us" << std::endl;
    std::cout << "Async Launch Time: " << tAsync.count() << " us" << std::endl;
    std::cout << "------------------------------------------------\n";

    // Stop Timers
    processTimerExit.store(true);
    if (timerThread.joinable()) timerThread.join();

    return 0;
}