#include "minidb/minidb.h"
#include <cassert>
#include <cstdio>
#include <array>
#include <algorithm>
#include <random>
#include <filesystem>
#include <fstream>
#include <map>
using namespace minidb;
int main() {
  std::remove("minidb_test.db");
  DiskManager disk("minidb_test.db"); BufferPoolManager bpm(disk);
  Schema schema({{"id",Column::Type::Int32},{"name",Column::Type::Varchar}});
  TableHeap heap(bpm); auto rid=heap.Insert(Tuple({std::int32_t(7),std::string("a")}));
  assert(heap.Get(rid).has_value()); assert(heap.Scan().size()==1);
  BPlusTree tree(bpm); assert(tree.Insert(7,rid)); assert(tree.Search(7)==rid);
  assert(tree.Range(1,9).size()==1);
  auto tokens=Lex("SELECT * FROM t"); assert(tokens[0].text=="SELECT");
  auto stmt=Parse("SELECT * FROM t"); assert(stmt.kind==Statement::Kind::Select);
  for (int i=0;i<150;++i) { auto r=heap.Insert(Tuple({std::int32_t(i),std::string("x")})); assert(tree.Insert(i+1000,r)); }
  assert(tree.Search(1149).has_value()); assert(tree.Range(1000,1149).size()==150);
  assert(!tree.Insert(1149, rid)); // duplicate keys are rejected
  assert(!tree.Search(-999).has_value());
  assert(tree.Range(9, 8).empty());
  assert(tree.Validate());
  page_id_t table_page=heap.FirstPageId(), root=tree.RootPageId(); bpm.FlushAllPages();
  { DiskManager reopened("minidb_test.db"); BufferPoolManager rbpm(reopened);
    TableHeap reloaded(rbpm, table_page); assert(reloaded.Scan().size()==151);
    BPlusTree reloaded_tree(rbpm, root); assert(reloaded_tree.Search(1149).has_value()); }

  // Exercise both insertion directions, random insertion, multiple leaf
  // splits, exact boundaries, and the on-disk structural checker.
  const std::string index_file="bplus_comprehensive_test.db";
  std::remove(index_file.c_str());
  {
    DiskManager index_disk(index_file); BufferPoolManager index_bpm(index_disk);
    BPlusTree index(index_bpm);
    assert(BPlusTree::LeafCapacity() == (PAGE_SIZE - 16) / 10);
    assert(BPlusTree::InternalCapacity() == (PAGE_SIZE - 20) / 8);
    std::map<std::int32_t, RID> reference;
    for (int key = 900; key >= 0; --key) {
      RID value{key + 1, static_cast<std::uint16_t>(key % 100)};
      assert(index.Insert(key, value));
      reference.emplace(key, value);
    }
    std::vector<int> keys;
    for (int key = 1001; key < 1500; ++key) keys.push_back(key);
    std::mt19937 generator(42);
    std::shuffle(keys.begin(), keys.end(), generator);
    for (int key : keys) {
      RID value{key + 1, static_cast<std::uint16_t>(key % 100)};
      assert(index.Insert(key, value));
      reference.emplace(key, value);
    }
    assert(index.Validate());
    assert(index.RootPageId() != INVALID_PAGE_ID);
    assert(index.Search(0)->page_id == 1);
    assert(index.Search(1499)->page_id == 1500);
    assert(!index.Search(1500).has_value());
    for (const auto& [key, value] : reference)
      assert(index.Search(key).value() == value);
    auto middle = index.Range(400, 1100);
    assert(middle.size() == 601);
    for (std::size_t i = 1; i < middle.size(); ++i)
      assert(middle[i - 1].first < middle[i].first);
    auto all = index.Range(0, 1499);
    assert(all.size() == 1400 && all.front().first == 0 && all.back().first == 1499);
    for (const auto& [key, value] : reference) {
      auto found = index.Range(key, key);
      const std::pair<std::int32_t, RID> expected{key, value};
      assert(found.size() == 1 && found.front() == expected);
    }
    const page_id_t persisted_root = index.RootPageId();
    index_bpm.FlushAllPages();
    {
      DiskManager reopened(index_file); BufferPoolManager reopened_bpm(reopened);
      BPlusTree reopened_index(reopened_bpm, persisted_root);
      assert(reopened_index.Validate());
      assert(reopened_index.Range(1200, 1205).size() == 6);
      assert(!reopened_index.Insert(1200, RID{77, 0}));
      for (const auto& [key, value] : reference)
        assert(reopened_index.Search(key).value() == value);
      const auto reopened_all = reopened_index.Range(0, 1499);
      assert(reopened_all.size() == reference.size());
      for (std::size_t i = 1; i < reopened_all.size(); ++i)
        assert(reopened_all[i - 1].first < reopened_all[i].first);
    }
  }
  std::remove(index_file.c_str());

  const std::string internal_split_file="bplus_internal_split_test.db";
  std::remove(internal_split_file.c_str());
  {
    DiskManager split_disk(internal_split_file); BufferPoolManager split_bpm(split_disk);
    BPlusTree split_tree(split_bpm);
    constexpr int key_count = 209000;
    for (int key = 0; key < key_count; ++key)
      assert(split_tree.Insert(key, RID{key + 10, static_cast<std::uint16_t>(key % 1000)}));
    assert(split_tree.Validate());
    assert(split_tree.Search(0)->page_id == 10);
    assert(split_tree.Search(key_count - 1)->page_id == key_count + 9);
    auto split_range = split_tree.Range(208990, 208999);
    assert(split_range.size() == 10 && split_range.front().first == 208990);
    split_bpm.FlushAllPages();
    const page_id_t split_root = split_tree.RootPageId();
    DiskManager reopened(internal_split_file); BufferPoolManager reopened_bpm(reopened);
    BPlusTree reopened_tree(reopened_bpm, split_root);
    assert(reopened_tree.Validate());
    assert(reopened_tree.Search(104857)->page_id == 104867);
  }
  std::remove(internal_split_file.c_str());
  Statement filtered=Parse("SELECT * FROM t WHERE id = 7 OR id = 8");
  Executor executor(heap,schema,&tree); assert(executor.Select(filtered).size()>=2);
  Planner planner(schema,&tree); assert(planner.Choose(filtered)==Planner::PlanKind::IndexScan);

  const std::string persistence_file="storage_persistence_test.db";
  std::remove(persistence_file.c_str());
  std::array<std::array<char,PAGE_SIZE>,3> expected{};
  {
    DiskManager persistent(persistence_file);
    std::array<page_id_t,3> ids{};
    for(std::size_t i=0;i<ids.size();++i) {
      ids[i]=persistent.AllocatePage();
      expected[i].fill(static_cast<char>('A'+i));
      assert(persistent.WritePage(ids[i],expected[i].data()));
    }
    assert(persistent.PageCount()==3);
    persistent.Flush();
  }
  {
    DiskManager persistent(persistence_file);
    assert(persistent.PageCount()==3);
    std::array<char,PAGE_SIZE> page{};
    for(std::size_t i=0;i<expected.size();++i) {
      assert(persistent.ReadPage(static_cast<page_id_t>(i),page.data()));
      assert(page==expected[i]);
    }
    assert(!persistent.ReadPage(3,page.data()));
  }
  std::remove(persistence_file.c_str());

  const std::string eviction_file="buffer_eviction_test.db";
  std::remove(eviction_file.c_str());
  {
    DiskManager eviction_disk(eviction_file);
    BufferPoolManager eviction_bpm(eviction_disk);
    std::array<page_id_t,11> ids{};
    for(auto& id:ids) {
      Page* page=eviction_bpm.NewPage(&id);
      assert(page!=nullptr);
      page->data[0]=static_cast<char>(id);
      assert(eviction_bpm.UnpinPage(id,true));
    }
    assert(BufferPoolManager::FrameCount()==10);
    assert(!eviction_bpm.IsPageResident(ids[0]));
    assert(eviction_bpm.IsPageResident(ids[1]));
    Page* pinned=eviction_bpm.FetchPage(ids[1]);
    assert(pinned!=nullptr);
    for(int i=0;i<10;++i) {
      page_id_t extra;
      Page* page=eviction_bpm.NewPage(&extra);
      assert(page!=nullptr);
      assert(eviction_bpm.UnpinPage(extra,false));
    }
    assert(eviction_bpm.IsPageResident(ids[1]));
    assert(eviction_bpm.UnpinPage(ids[1],false));
  }
  std::remove(eviction_file.c_str());

  const std::string wal_file="wal_test.log";
  const std::string wal_db="wal_test.db";
  std::remove(wal_file.c_str());
  std::remove(wal_db.c_str());
  page_id_t wal_rid_page=INVALID_PAGE_ID;
  {
    LogManager wal(wal_file);
    DiskManager wal_disk(wal_db);
    BufferPoolManager wal_bpm(wal_disk, &wal);
    TransactionManager transactions(wal);
    const auto txn=transactions.BeginTransaction();
    TableHeap wal_heap(wal_bpm);
    const RID rid=wal_heap.Insert(Tuple({std::int32_t(1),std::string("before")}));
    wal_rid_page=rid.page_id;
    transactions.LogInsert(txn, rid, Tuple({std::int32_t(1),std::string("before")}));
    const auto before=wal_heap.Get(rid).value();
    const Tuple after({std::int32_t(2),std::string("after")});
    assert(wal_heap.Update(rid, after));
    transactions.LogUpdate(txn, rid, before, after);
    transactions.CommitTransaction(txn);
    wal_bpm.FlushAllPages();
    assert(transactions.State(txn)==TransactionState::Committed);
  }
  {
    LogManager wal(wal_file);
    assert(wal.ReadAll().size()==4);
    DiskManager wal_disk(wal_db);
    BufferPoolManager wal_bpm(wal_disk, &wal);
    TableHeap wal_heap(wal_bpm, wal_rid_page);
    RecoveryManager recovery(wal);
    assert(recovery.Losers().empty());
    assert(recovery.Redo().size()==2);
    assert(recovery.Recover(wal_heap)==1);
    assert(wal_heap.Get(RID{wal_rid_page,0})->ToString()=="2,after");
  }
  const std::string loser_db="wal_loser.db";
  std::remove(loser_db.c_str());
  {
    LogManager wal(wal_file);
    DiskManager loser_disk(loser_db);
    BufferPoolManager loser_bpm(loser_disk, &wal);
    TransactionManager transactions(wal);
    const auto txn=transactions.BeginTransaction();
    TableHeap loser_heap(loser_bpm);
    const RID rid=loser_heap.Insert(Tuple({std::int32_t(9),std::string("loser")}));
    transactions.LogInsert(txn, rid, Tuple({std::int32_t(9),std::string("loser")}));
    transactions.Flush();
    assert(transactions.State(txn)==TransactionState::Active);
    const auto aborted=transactions.BeginTransaction();
    transactions.LogInsert(aborted, RID{77, 0},
                            Tuple({std::int32_t(8),std::string("aborted")}));
    transactions.AbortTransaction(aborted);
    assert(transactions.State(aborted)==TransactionState::Aborted);
  }
  {
    LogManager wal(wal_file);
    RecoveryManager recovery(wal);
    const auto losers=recovery.Losers();
    assert(losers.size()==1);
    assert(recovery.Redo().size()==2);
  }
  std::size_t wal_records_before_corrupt=0;
  {
    LogManager wal(wal_file);
    wal_records_before_corrupt=wal.ReadAll().size();
  }
  const auto wal_size=std::filesystem::file_size(wal_file);
  {
    std::ofstream corrupt(wal_file, std::ios::binary|std::ios::app);
    const char bytes[]={'x','y','z'};
    corrupt.write(bytes, sizeof(bytes));
  }
  {
    LogManager wal(wal_file);
    assert(wal.ReadAll().size()==wal_records_before_corrupt);
    assert(std::filesystem::file_size(wal_file)==wal_size);
  }
  std::remove(wal_file.c_str());
  std::remove(wal_db.c_str());
  std::remove(loser_db.c_str());

  const auto select_all = Parse("SELECT * FROM users;");
  assert(select_all.columns == std::vector<std::string>{"*"});
  assert(select_all.table == "users");
  assert(std::get<SelectStatement>(select_all.ast).where == nullptr);
  const auto select_columns = Parse("SELECT id, name FROM users");
  const std::vector<std::string> selected_columns{"id", "name"};
  assert(select_columns.columns == selected_columns);
  const auto equality = Parse("SELECT id FROM users WHERE id = 10");
  const auto &equality_ast = std::get<SelectStatement>(equality.ast);
  assert(equality_ast.where->kind == ExpressionKind::Comparison);
  assert(equality_ast.where->text == "=");
  assert(equality_ast.where->left->kind == ExpressionKind::Column);
  assert(equality_ast.where->right->integer == 10);
  assert(Parse("SELECT id FROM users WHERE id != 10").predicates[0].op == "!=");
  assert(Parse("SELECT id FROM users WHERE id < 10").predicates[0].op == "<");
  assert(Parse("SELECT id FROM users WHERE id > 10").predicates[0].op == ">");
  assert(Parse("SELECT id FROM users WHERE id <= 10").predicates[0].op == "<=");
  assert(Parse("SELECT id FROM users WHERE id >= 10").predicates[0].op == ">=");
  const auto precedence = Parse(
      "SELECT id FROM users WHERE age >= 18 OR age < 30 AND age != 25");
  const auto &precedence_root = std::get<SelectStatement>(precedence.ast).where;
  assert(precedence_root->kind == ExpressionKind::Or);
  assert(precedence_root->right->kind == ExpressionKind::And);
  const auto parenthesized = Parse(
      "SELECT id FROM users WHERE (age >= 18 OR age < 30) AND age != 25");
  assert(std::get<SelectStatement>(parenthesized.ast).where->kind == ExpressionKind::And);
  const auto insert_ast = Parse("INSERT INTO users VALUES (10, 'Ada');");
  const std::vector<std::string> inserted_values{"10", "Ada"};
  assert(insert_ast.values == inserted_values);
  assert(std::get<InsertStatement>(insert_ast.ast).table == "users");
  const auto update_ast = Parse("UPDATE users SET name = 'Bob' WHERE id >= 10;");
  assert(update_ast.update_column == "name");
  assert(update_ast.update_value == "Bob");
  assert(std::get<UpdateStatement>(update_ast.ast).where->text == ">=");
  const auto sql_tokens = Lex("SELECT id FROM users WHERE id >= 10 AND name != 'x';");
  assert(sql_tokens[0].kind == TokenKind::Keyword);
  assert(sql_tokens[6].kind == TokenKind::GreaterEqual);
  assert(sql_tokens[8].kind == TokenKind::And);
  assert(sql_tokens[10].kind == TokenKind::NotEqual);
  assert(sql_tokens.back().kind == TokenKind::End);
  const std::vector<std::string> invalid_sql = {
      "", "SELECT FROM users", "SELECT id", "SELECT id FROM",
      "SELECT id, FROM users", "SELECT id FROM users WHERE id =",
      "SELECT id FROM users WHERE id ! 10", "SELECT id FROM users garbage",
      "INSERT INTO users VALUES ()", "INSERT INTO users VALUES (1,)",
      "UPDATE users SET name", "UPDATE users SET name =",
      "UPDATE users SET name = 'x' WHERE", "SELECT id FROM users; garbage"};
  for (const auto &sql : invalid_sql) {
    bool rejected = false;
    try { (void)Parse(sql); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
  }
  bool bad_character = false;
  try { (void)Lex("SELECT @ FROM users"); } catch (const std::runtime_error &) { bad_character = true; }
  assert(bad_character);
  bool bad_string = false;
  try { (void)Lex("SELECT 'unterminated"); } catch (const std::runtime_error &) { bad_string = true; }
  assert(bad_string);

  const std::string exec_db = "execution_test.db";
  const std::string exec_wal = "execution_test.wal";
  std::remove(exec_db.c_str());
  std::remove(exec_wal.c_str());
  {
    LogManager log(exec_wal);
    DiskManager execution_disk(exec_db);
    BufferPoolManager execution_bpm(execution_disk, &log);
    Schema execution_schema({{"id", Column::Type::Int32}, {"name", Column::Type::Varchar}});
    TableHeap execution_heap(execution_bpm);
    BPlusTree execution_index(execution_bpm);
    TransactionManager execution_transactions(log);
    Executor execution(execution_heap, execution_schema, &execution_index, "users",
                       &execution_transactions);
    Planner execution_planner(execution_schema, &execution_index, "users");
    for (int id = 0; id < 520; ++id) {
      const auto statement = Parse("INSERT INTO users VALUES (" + std::to_string(id) +
                                   ", 'user" + std::to_string(id) + "');");
      const auto result = execution.Execute(statement);
      assert(result.rows.size() == 1);
    }
    assert(execution.ExecuteSql("SELECT * FROM users;").rows.size() == 520);
    const auto projected = execution.ExecuteSql("SELECT name, id FROM users WHERE id = 10;");
    const std::vector<std::string> projected_columns{"name", "id"};
    assert(projected.columns == projected_columns);
    assert(projected.rows.size() == 1);
    assert(std::get<std::string>(projected.rows[0][0]) == "user10");
    assert(std::get<std::int32_t>(projected.rows[0][1]) == 10);
    const auto predicates = {
        "SELECT id FROM users WHERE id = 10;",
        "SELECT id FROM users WHERE id != 10;",
        "SELECT id FROM users WHERE id < 10;",
        "SELECT id FROM users WHERE id > 510;",
        "SELECT id FROM users WHERE id <= 10;",
        "SELECT id FROM users WHERE id >= 510;"};
    for (const auto sql : predicates) {
      const auto statement = Parse(sql);
      assert(execution_planner.Plan(statement).kind == PlanKind::IndexScan);
      assert(!execution.Execute(statement).rows.empty());
    }
    const auto range = execution.ExecuteSql(
        "SELECT id FROM users WHERE id >= 100 AND id <= 200;");
    assert(range.rows.size() == 101);
    const auto or_result = execution.ExecuteSql(
        "SELECT id FROM users WHERE id = 10 OR id = 20;");
    assert(or_result.rows.size() == 2);
    const auto seq_statement = Parse("SELECT id FROM users WHERE name = 'user10';");
    assert(execution_planner.Plan(seq_statement).kind == PlanKind::SequentialScan);
    assert(execution.Execute(seq_statement).rows.size() == 1);
    const auto update = execution.ExecuteSql(
        "UPDATE users SET name = 'changed' WHERE id >= 10 AND id < 20;");
    assert(std::get<std::int32_t>(update.rows[0][0]) == 10);
    assert(std::get<std::string>(execution.ExecuteSql(
        "SELECT name FROM users WHERE id = 15;").rows[0][0]) == "changed");
    bool rejected_index_update = false;
    try { (void)execution.ExecuteSql("UPDATE users SET id = 99 WHERE id = 10;"); }
    catch (const std::runtime_error &) { rejected_index_update = true; }
    assert(rejected_index_update);
    bool rejected_unknown = false;
    try { (void)execution.ExecuteSql("SELECT missing FROM users;"); }
    catch (const std::runtime_error &) { rejected_unknown = true; }
    assert(rejected_unknown);
    execution_bpm.FlushAllPages();
    const auto root = execution_index.RootPageId();
    const auto first_page = execution_heap.FirstPageId();
    {
      DiskManager reopened_disk(exec_db);
      BufferPoolManager reopened_bpm(reopened_disk, &log);
      TableHeap reopened_heap(reopened_bpm, first_page);
      BPlusTree reopened_index(reopened_bpm, root);
      Executor reopened(reopened_heap, execution_schema, &reopened_index, "users");
      assert(reopened.ExecuteSql("SELECT id FROM users WHERE id >= 500;").rows.size() == 20);
      assert(std::get<std::string>(reopened.ExecuteSql(
          "SELECT name FROM users WHERE id = 15;").rows[0][0]) == "changed");
    }
  }
  std::remove(exec_db.c_str());
  std::remove(exec_wal.c_str());

  const std::filesystem::path catalog_root = "minidb_catalog_test";
  std::filesystem::remove_all(catalog_root);
  {
    Database database(catalog_root);
    assert(database.ExecuteSql("CREATE DATABASE college;").rows.size() == 1);
    database.ExecuteSql("USE college;");
    database.ExecuteSql(
        "CREATE TABLE students (id INT PRIMARY KEY, name VARCHAR(40) NOT NULL, age INT, active BOOLEAN);");
    const auto inserted = database.ExecuteSql(
        "INSERT INTO students VALUES (1, 'Amit', 21, TRUE), (2, 'Sara', 22, FALSE);");
    assert(std::get<std::int32_t>(inserted.rows[0][0]) == 2);
    assert(database.ExecuteSql("SELECT * FROM students;").rows.size() == 2);
    const auto ordered = database.ExecuteSql(
        "SELECT id, name FROM students ORDER BY id DESC LIMIT 1;");
    assert(ordered.rows.size() == 1);
    assert(std::get<std::int32_t>(ordered.rows[0][0]) == 2);
    const auto count = database.ExecuteSql("SELECT COUNT(*) FROM students;");
    assert(std::get<std::int32_t>(count.rows[0][0]) == 2);
    database.ExecuteSql("UPDATE students SET age = 23 WHERE id = 1;");
    database.ExecuteSql("UPDATE students SET active = FALSE WHERE id = 1;");
    assert(database.ExecuteSql("DELETE FROM students WHERE id = 2;").rows.size() == 1);
    database.ExecuteSql("ALTER TABLE students ADD COLUMN city VARCHAR(20);");
    assert(database.Describe("students").schema.Columns().size() == 5);
  }
  {
    Database reopened(catalog_root);
    reopened.ExecuteSql("USE college;");
    const auto result = reopened.ExecuteSql("SELECT name, age, city FROM students WHERE id = 1;");
    assert(result.rows.size() == 1);
    assert(std::get<std::string>(result.rows[0][0]) == "Amit");
    assert(std::get<std::int32_t>(result.rows[0][1]) == 23);
    assert(std::get<std::string>(result.rows[0][2]).empty());
    reopened.ExecuteSql("DROP TABLE students;");
    reopened.ExecuteSql("DROP DATABASE college;");
    assert(reopened.ShowDatabases().empty());
  }
  std::filesystem::remove_all(catalog_root);
  return 0;
}
