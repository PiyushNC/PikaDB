#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <variant>
#include <map>
#include <memory>
#include <filesystem>

namespace minidb {
constexpr std::size_t PAGE_SIZE = 4096;
static_assert(PAGE_SIZE == 4096);
using page_id_t = std::int32_t;
constexpr page_id_t INVALID_PAGE_ID = -1;
using frame_id_t = std::int32_t;
using Value = std::variant<std::int32_t, std::string, bool>;
class LogManager;

class DiskManager {
 public:
  explicit DiskManager(const std::string &file);
  ~DiskManager();
  page_id_t AllocatePage();
  bool ReadPage(page_id_t id, char *out);
  bool WritePage(page_id_t id, const char *data);
  void Flush();
  page_id_t PageCount() const { return next_; }
 private:
  std::fstream file_;
  page_id_t next_{0};
};

struct Page {
  page_id_t id{INVALID_PAGE_ID};
  std::array<char, PAGE_SIZE> data{};
  int pin_count{0};
  bool dirty{false};
};
static_assert(sizeof(std::array<char, PAGE_SIZE>) == PAGE_SIZE);

class BufferPoolManager {
 public:
  static constexpr std::size_t POOL_SIZE = 10;
  explicit BufferPoolManager(DiskManager &disk, LogManager *wal = nullptr);
  void SetLogManager(LogManager *wal) { wal_ = wal; }
  Page *NewPage(page_id_t *id);
  Page *FetchPage(page_id_t id);
  bool UnpinPage(page_id_t id, bool dirty);
  bool DeletePage(page_id_t id);
  void FlushAllPages();
  std::size_t Size() const { return page_table_.size(); }
  static constexpr std::size_t FrameCount() { return POOL_SIZE; }
  bool IsPageResident(page_id_t id) const { return page_table_.find(id) != page_table_.end(); }
 private:
  frame_id_t Victim();
  void Touch(frame_id_t);
  void RemoveFromLru(frame_id_t);
  void AddToLru(frame_id_t);
  void FlushWalBeforeData();
  DiskManager &disk_;
  LogManager *wal_{nullptr};
  std::array<Page, POOL_SIZE> pages_{};
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::list<frame_id_t> lru_;
  std::array<std::list<frame_id_t>::iterator, POOL_SIZE> positions_{};
  std::array<bool, POOL_SIZE> evictable_{};
};

struct RID { page_id_t page_id{INVALID_PAGE_ID}; std::uint16_t slot{0}; friend bool operator==(const RID&,const RID&)=default; };
struct Column {
  std::string name;
  enum class Type { Int32, Varchar, Boolean };
  Type type;
  bool not_null{false};
  bool primary_key{false};
};
class Schema {
 public:
  Schema() = default;
  explicit Schema(std::vector<Column> columns): columns_(std::move(columns)) {}
  const std::vector<Column>& Columns() const { return columns_; }
  int IndexOf(std::string_view name) const;
 private: std::vector<Column> columns_;
};
class Tuple {
 public:
  Tuple() = default;
  explicit Tuple(std::vector<Value> values): values_(std::move(values)) {}
  const std::vector<Value>& Values() const { return values_; }
  const Value& Get(std::size_t i) const { return values_.at(i); }
  std::string ToString() const;
 private: std::vector<Value> values_;
};
class TableHeap {
 public:
  explicit TableHeap(BufferPoolManager &bpm, page_id_t first_page = INVALID_PAGE_ID);
  RID Insert(const Tuple &tuple);
  std::optional<Tuple> Get(const RID &rid) const;
  bool Update(const RID &rid, const Tuple &tuple);
  bool Delete(const RID &rid);
  std::vector<std::pair<RID,Tuple>> Scan() const;
  page_id_t FirstPageId() const { return page_; }
 private:
  void Load();
  void FlushPage(page_id_t);
  BufferPoolManager &bpm_;
  page_id_t page_{INVALID_PAGE_ID};
  std::vector<Tuple> tuples_;
  std::vector<page_id_t> tuple_pages_;
  std::vector<bool> deleted_;
};
class SeqIterator {
 public:
  explicit SeqIterator(const TableHeap &heap);
  bool HasNext() const;
  std::pair<RID,Tuple> Next();
 private:
  std::vector<std::pair<RID,Tuple>> rows_; std::size_t pos_{0};
};

class BPlusTree {
 public:
  BPlusTree(BufferPoolManager &bpm, page_id_t root = INVALID_PAGE_ID);
  bool Insert(std::int32_t key, RID value);
  std::optional<RID> Search(std::int32_t key) const;
  std::vector<std::pair<std::int32_t,RID>> Range(std::int32_t low, std::int32_t high) const;
  // Checks every reachable page and the leaf chain.  Intended for tests and
  // diagnostics; malformed on-disk pages cause this to return false.
  bool Validate() const;
  page_id_t RootPageId() const { return root_; }
  // Nodes have a 16-byte header (magic, count, parent, next).  Leaf entries
  // are key/RID (10 bytes); internal entries are a key plus a child pointer.
  static constexpr std::size_t LeafCapacity() { return (PAGE_SIZE - 16) / (sizeof(std::int32_t) + sizeof(page_id_t) + sizeof(std::uint16_t)); }
  static constexpr std::size_t InternalCapacity() { return (PAGE_SIZE - 16 - sizeof(page_id_t)) / (sizeof(std::int32_t) + sizeof(page_id_t)); }
 private:
  struct Node { bool leaf; page_id_t parent{INVALID_PAGE_ID}, next{INVALID_PAGE_ID}; std::vector<std::int32_t> keys; std::vector<RID> values; std::vector<page_id_t> children; };
  Node Read(page_id_t) const;
  void Write(page_id_t, const Node&) const;
  page_id_t NewNode(const Node&);
  std::optional<std::pair<std::int32_t,page_id_t>> InsertRec(page_id_t,std::int32_t,RID);
  void InsertParent(page_id_t,std::int32_t,page_id_t);
  void Load();
  BufferPoolManager &bpm_; page_id_t root_;
};

enum class LogType : std::uint8_t { Begin, Insert, Update, Commit, Abort };
struct LogRecord {
  LogType type{LogType::Begin};
  std::uint64_t txn{0};
  RID rid{};
  std::string before, after;
};
class LogManager {
 public:
  explicit LogManager(const std::string &file);
  void Append(const LogRecord &record);
  void AppendBegin(std::uint64_t txn);
  void AppendInsert(std::uint64_t txn, RID rid, const Tuple &tuple);
  void AppendUpdate(std::uint64_t txn, RID rid, const Tuple &before,
                    const Tuple &after);
  void AppendCommit(std::uint64_t txn);
  void AppendAbort(std::uint64_t txn);
  void Flush();
  std::vector<LogRecord> ReadAll() const;
 private:
  void Load();
  std::string file_;
  std::vector<LogRecord> records_;
};
enum class TransactionState { Active, Committed, Aborted };
class TransactionManager {
 public:
  explicit TransactionManager(LogManager &log);
  std::uint64_t BeginTransaction();
  void CommitTransaction(std::uint64_t txn);
  void AbortTransaction(std::uint64_t txn);
  void LogInsert(std::uint64_t txn, RID rid, const Tuple &tuple);
  void LogUpdate(std::uint64_t txn, RID rid, const Tuple &before,
                 const Tuple &after);
  TransactionState State(std::uint64_t txn) const;
  void Flush();
 private:
  LogManager &log_;
  std::uint64_t next_{1};
  std::unordered_map<std::uint64_t, TransactionState> states_;
};
class RecoveryManager {
 public:
  explicit RecoveryManager(LogManager &log): log_(log) {}
  std::vector<LogRecord> Redo() const;
  std::vector<std::uint64_t> Losers() const;
  std::size_t Recover(TableHeap &heap) const;
 private: LogManager &log_;
};

enum class TokenKind {
  Identifier, Integer, String, Comma, Star, Equal, NotEqual, Less, Greater,
  LessEqual, GreaterEqual, And, Or, Keyword, LParen, RParen, Semicolon, End
};
struct Token { TokenKind kind; std::string text; };
std::vector<Token> Lex(std::string_view sql);
enum class ExpressionKind { Column, IntegerLiteral, StringLiteral, Comparison, And, Or };
struct Expression {
  ExpressionKind kind;
  std::string text;
  std::int32_t integer{0};
  std::shared_ptr<Expression> left;
  std::shared_ptr<Expression> right;
};
using ExpressionPtr = std::shared_ptr<Expression>;
struct SelectStatement { std::vector<std::string> columns; std::string table; ExpressionPtr where; };
struct InsertStatement { std::string table; std::vector<std::string> values; };
struct UpdateStatement {
  std::string table, column, value;
  ExpressionPtr where;
};
struct Predicate { std::string column, op, value; };
struct Statement {
  enum class Kind { Select, Insert, Update };
  Kind kind; std::vector<std::string> columns, values; std::string table;
  std::vector<Predicate> predicates; std::vector<bool> predicate_or;
  std::string update_column, update_value;
  std::variant<SelectStatement, InsertStatement, UpdateStatement> ast;
};
Statement Parse(std::string_view sql);

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};
enum class PlanKind { SequentialScan, IndexScan };
struct QueryPlan {
  PlanKind kind{PlanKind::SequentialScan};
  bool use_filter{false};
  bool use_projection{false};
};
class Executor {
 public:
  Executor(TableHeap &heap, const Schema &schema, BPlusTree *index = nullptr,
           std::string table_name = "t", TransactionManager *transactions = nullptr);
  QueryResult Execute(const Statement &stmt, const QueryPlan &plan = {}) const;
  QueryResult ExecuteSql(std::string_view sql, const QueryPlan &plan = {}) const;
  std::vector<Tuple> Select(const Statement &stmt) const;
  RID Insert(const Statement &stmt);
  std::size_t Update(const Statement &stmt);
  std::size_t Delete(const Statement &stmt);
 private:
  bool Matches(const Tuple&, const ExpressionPtr&) const;
  Tuple MakeTuple(const std::vector<std::string>& values) const;
  void CheckTable(std::string_view table) const;
  TableHeap &heap_; const Schema &schema_; BPlusTree *index_;
  std::string table_name_; TransactionManager *transactions_;
};
class Planner {
 public:
  using PlanKind = minidb::PlanKind;
  Planner(const Schema &schema, const BPlusTree *index, std::string table_name = "t")
      : schema_(schema), index_(index), table_name_(std::move(table_name)) {}
  QueryPlan Plan(const Statement &stmt) const;
  PlanKind Choose(const Statement &stmt) const { return Plan(stmt).kind; }
 private:
  const Schema &schema_; const BPlusTree *index_; std::string table_name_;
};

struct TableDefinition {
  std::string name;
  Schema schema;
};

class Database {
 public:
  explicit Database(std::filesystem::path data_directory = "data");
  ~Database();
  std::vector<std::string> ShowDatabases() const;
  void CreateDatabase(const std::string &name);
  void DropDatabase(const std::string &name);
  void UseDatabase(const std::string &name);
  bool HasDatabase() const { return !current_database_.empty(); }
  const std::string &CurrentDatabase() const { return current_database_; }
  void CreateTable(const TableDefinition &definition);
  void AddColumn(const std::string &table, Column column);
  void DropTable(const std::string &name);
  std::vector<std::string> ShowTables() const;
  TableDefinition Describe(const std::string &name) const;
  QueryResult ExecuteSql(std::string_view sql);
  void Flush();
 private:
  struct TableContext;
  void RequireDatabase() const;
  TableContext &OpenTable(const std::string &name);
  void LoadCatalog();
  void SaveCatalog() const;
  std::filesystem::path data_directory_;
  std::string current_database_;
  std::unordered_map<std::string, TableDefinition> catalog_;
  std::unordered_map<std::string, std::unique_ptr<TableContext>> tables_;
};

class Shell {
 public:
  explicit Shell(std::filesystem::path data_directory = "data")
      : database_(std::move(data_directory)) {}
  int Run(std::istream &input, std::ostream &output);
 private:
  Database database_;
};
}  // namespace minidb
