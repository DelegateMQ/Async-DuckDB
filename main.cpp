// Asynchronous DuckDB API's implemented using C++ delegates
// @see https://github.com/endurodave/Async-DuckDB
// @see https://github.com/endurodave/DelegateMQ
// David Lafreniere, 2026.

#include "DelegateMQ.h"
#include "DuckDB/async_duckdb.hpp"
#include "UnitTest/async_duckdb_ut.h"
#include "Examples/Examples.h"
#include <stdio.h>
#include <iostream>
#include <filesystem>

void DeleteDatabaseFiles() {
    try {
        std::filesystem::remove("simple.db");
        std::filesystem::remove("futures.db");
        std::filesystem::remove("prepared.db");
        std::filesystem::remove("appender.db");
        std::filesystem::remove("transactions.db");
    } catch (...) {}
}

int main(void)
{
    DeleteDatabaseFiles();
    try {
        async::init_worker();
        
        std::cout << "=== Starting Async-DuckDB Examples ===\n";
        RunSimpleExample();
        RunFutureExample();
        RunPreparedStatementExample();
        RunAppenderExample();
        RunTransactionExample();
        RunStressTest();
        std::cout << "\n=== All Examples Completed ===\n";

        int ret = RunUnitTests();
        
        async::shutdown_worker();
        
        return ret;
    }
    catch (const std::exception& e) {
        std::cerr << "Unhandled Exception: " << e.what() << std::endl;
        return 1;
    }
}
