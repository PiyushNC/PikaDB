#include "minidb/minidb.h"

#include <cstdio>
#include <iostream>

using namespace minidb;

int main() {
  const std::string database = "minidb_demo.db";
  const std::string wal_file = "minidb_demo.wal";
  std::remove(database.c_str());
  std::remove(wal_file.c_str());

  const Schema schema({{"id", Column::Type::Int32},
                       {"name", Column::Type::Varchar}});
  page_id_t table_page = INVALID_PAGE_ID;
  page_id_t root_page = INVALID_PAGE_ID;

  {
    LogManager log(wal_file);
    DiskManager disk(database);
    BufferPoolManager buffer_pool(disk, &log);
    TableHeap heap(buffer_pool);
    BPlusTree index(buffer_pool);
    TransactionManager transactions(log);
    Executor executor(heap, schema, &index, "users", &transactions);

    executor.ExecuteSql("INSERT INTO users VALUES (10, 'Ada');");
    executor.ExecuteSql("INSERT INTO users VALUES (20, 'Grace');");
    executor.ExecuteSql("INSERT INTO users VALUES (30, 'Linus');");

    const auto exact = executor.ExecuteSql(
        "SELECT name FROM users WHERE id = 20;");
    std::cout << "indexed lookup: "
              << std::get<std::string>(exact.rows.front().front()) << '\n';

    const auto range = executor.ExecuteSql(
        "SELECT id, name FROM users WHERE id >= 10 AND id <= 20;");
    std::cout << "range rows: " << range.rows.size() << '\n';

    executor.ExecuteSql("UPDATE users SET name = 'Grace Hopper' WHERE id = 20;");
    table_page = heap.FirstPageId();
    root_page = index.RootPageId();
    buffer_pool.FlushAllPages();
  }

  {
    LogManager log(wal_file);
    DiskManager disk(database);
    BufferPoolManager buffer_pool(disk, &log);
    TableHeap heap(buffer_pool, table_page);
    BPlusTree index(buffer_pool, root_page);
    Executor executor(heap, schema, &index, "users");
    const auto persisted = executor.ExecuteSql(
        "SELECT name FROM users WHERE id = 20;");
    std::cout << "after reopen: "
              << std::get<std::string>(persisted.rows.front().front()) << '\n';
  }

  std::remove(database.c_str());
  std::remove(wal_file.c_str());
  return 0;
}
