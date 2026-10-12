#include "minidb/minidb.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

using namespace minidb;
using Clock = std::chrono::steady_clock;

static double Seconds(Clock::duration duration) {
  return std::chrono::duration<double>(duration).count();
}

int main() {
  constexpr int row_count = 1000;
  constexpr int lookup_count = 5000;
  const std::string database = "minidb_benchmark.db";
  const std::string wal_file = "minidb_benchmark.wal";
  std::remove(database.c_str());
  std::remove(wal_file.c_str());

  {
    const Schema schema({{"id", Column::Type::Int32},
                         {"name", Column::Type::Varchar}});
    LogManager log(wal_file);
    DiskManager disk(database);
    BufferPoolManager buffer_pool(disk, &log);
    TableHeap heap(buffer_pool);
    BPlusTree index(buffer_pool);
    TransactionManager transactions(log);
    Executor executor(heap, schema, &index, "bench", &transactions);

  auto start = Clock::now();
  for (int id = 0; id < row_count; ++id)
    executor.ExecuteSql("INSERT INTO bench VALUES (" + std::to_string(id) +
                        ", 'row');");
  const auto insert_time = Seconds(Clock::now() - start);

  start = Clock::now();
  for (int i = 0; i < lookup_count; ++i)
    (void)executor.ExecuteSql("SELECT id FROM bench WHERE id = " +
                              std::to_string(i % row_count) + ";");
  const auto lookup_time = Seconds(Clock::now() - start);

  start = Clock::now();
  std::size_t range_rows = 0;
  for (int i = 0; i < 100; ++i)
    range_rows += executor.ExecuteSql(
        "SELECT id FROM bench WHERE id >= 100 AND id <= 599;").rows.size();
  const auto range_time = Seconds(Clock::now() - start);

  start = Clock::now();
  std::size_t sequential_rows = 0;
  for (int i = 0; i < 10; ++i) sequential_rows += heap.Scan().size();
  const auto sequential_time = Seconds(Clock::now() - start);

  start = Clock::now();
  std::size_t fetched_pages = 0;
  const auto records = heap.Scan();
  for (const auto &[rid, tuple] : records) {
    (void)tuple;
    if (auto *page = buffer_pool.FetchPage(rid.page_id)) {
      ++fetched_pages;
      buffer_pool.UnpinPage(rid.page_id, false);
    }
  }
  const auto pressure_time = Seconds(Clock::now() - start);

  buffer_pool.FlushAllPages();
  std::cout << "MiniDB benchmark (rows=" << row_count
            << ", frames=" << BufferPoolManager::FrameCount() << ")\n"
            << "INSERT: " << insert_time << " s, "
            << row_count / insert_time << " rows/s\n"
            << "EXACT LOOKUP: " << lookup_time << " s, "
            << lookup_count / lookup_time << " ops/s\n"
            << "RANGE SCAN: " << range_time << " s, "
            << range_rows / range_time << " rows/s\n"
            << "SEQUENTIAL SCAN: " << sequential_time << " s, "
            << sequential_rows / sequential_time << " rows/s\n"
            << "BUFFER PRESSURE: " << pressure_time << " s, "
            << fetched_pages / pressure_time << " page fetches/s\n";

  }
  std::remove(database.c_str());
  std::remove(wal_file.c_str());
  return 0;
}
