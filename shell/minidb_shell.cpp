#include "minidb/minidb.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <type_traits>

namespace {
std::string Upper(std::string value) {
  for (char &c : value)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return value;
}

std::vector<std::string> Words(const std::vector<minidb::Token> &tokens) {
  std::vector<std::string> result;
  for (const auto &token : tokens)
    if (token.kind != minidb::TokenKind::End &&
        token.kind != minidb::TokenKind::Semicolon)
      result.push_back(Upper(token.text));
  return result;
}

void PrintResult(const minidb::QueryResult &result, std::ostream &out) {
  if (result.columns.empty()) {
    out << "Statement executed.\n";
    return;
  }
  for (const auto &column : result.columns) out << column << '\t';
  out << '\n';
  for (const auto &row : result.rows) {
    for (const auto &value : row) {
      std::visit([&out](const auto &item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) out << (item ? "TRUE" : "FALSE") << '\t';
        else out << item << '\t';
      }, value);
    }
    out << '\n';
  }
  out << result.rows.size() << " row(s).\n";
}

void PrintHelp(std::ostream &out) {
  out << "MiniDB SQL commands:\n"
      << "  CREATE DATABASE name;\n"
      << "  SHOW DATABASES;\n"
      << "  USE name;\n"
      << "  DROP DATABASE name;\n"
      << "  CREATE TABLE name (id INT PRIMARY KEY, name VARCHAR NOT NULL, active BOOLEAN);\n"
      << "  SHOW TABLES;\n"
      << "  DESCRIBE name;\n"
      << "  DROP TABLE name;\n"
      << "  INSERT INTO name VALUES (...);\n"
      << "  SELECT ... FROM name [WHERE ...];\n"
      << "  UPDATE name SET column = value [WHERE ...];\n"
      << "  DELETE FROM name [WHERE ...];\n"
      << "  HELP;  EXIT;\n";
}

void HandleCreateTable(const std::vector<minidb::Token> &tokens,
                       minidb::Database &database) {
  std::size_t p = 0;
  auto word = [&]() -> std::string {
    if (p >= tokens.size()) throw std::runtime_error("incomplete CREATE TABLE");
    return Upper(tokens[p++].text);
  };
  if (word() != "CREATE" || word() != "TABLE") throw std::runtime_error("invalid CREATE TABLE");
  const std::string name = tokens[p++].text;
  if (tokens[p++].kind != minidb::TokenKind::LParen)
    throw std::runtime_error("CREATE TABLE requires '('");
  std::vector<minidb::Column> columns;
  while (p < tokens.size() && tokens[p].kind != minidb::TokenKind::RParen) {
    const std::string column_name = tokens[p++].text;
    const std::string type = word();
    minidb::Column column{column_name, type == "INT" ? minidb::Column::Type::Int32
                             : type == "BOOLEAN" ? minidb::Column::Type::Boolean
                             : minidb::Column::Type::Varchar};
    if (type != "INT" && type != "VARCHAR" && type != "BOOLEAN")
      throw std::runtime_error("supported types are INT, VARCHAR, and BOOLEAN");
    if (p < tokens.size() && tokens[p].kind == minidb::TokenKind::LParen) {
      ++p;
      if (p < tokens.size()) ++p;
      if (p >= tokens.size() || tokens[p].kind != minidb::TokenKind::RParen)
        throw std::runtime_error("malformed VARCHAR length");
      ++p;
    }
    while (p < tokens.size() && tokens[p].kind != minidb::TokenKind::Comma &&
           tokens[p].kind != minidb::TokenKind::RParen) {
      const std::string constraint = word();
      if (constraint == "NOT") {
        if (word() != "NULL") throw std::runtime_error("expected NULL after NOT");
        column.not_null = true;
      } else if (constraint == "PRIMARY") {
        if (word() != "KEY") throw std::runtime_error("expected KEY after PRIMARY");
        column.primary_key = true;
      } else {
        throw std::runtime_error("unsupported column constraint: " + constraint);
      }
    }
    columns.push_back(column);
    if (p < tokens.size() && tokens[p].kind == minidb::TokenKind::Comma) ++p;
  }
  if (p >= tokens.size() || tokens[p].kind != minidb::TokenKind::RParen)
    throw std::runtime_error("CREATE TABLE requires ')'");
  database.CreateTable({name, minidb::Schema(std::move(columns))});
}
}  // namespace

int minidb::Shell::Run(std::istream &input, std::ostream &output) {
  output << "MiniDB> ";
  std::string line;
  std::string statement;
  while (std::getline(input, line)) {
    statement += line;
    statement += '\n';
    if (statement.find(';') == std::string::npos) continue;
    const auto tokens = Lex(statement);
    const auto words = Words(tokens);
    try {
      if (words.empty()) {
        statement.clear();
        output << "MiniDB> ";
        continue;
      }
      if (words[0] == "EXIT" || words[0] == "QUIT") break;
      if (words[0] == "HELP") {
        PrintHelp(output);
      } else if (words[0] == "CREATE" && words.size() >= 2 &&
                 words[1] == "DATABASE") {
        database_.CreateDatabase(tokens[2].text);
        output << "Database created.\n";
      } else if (words[0] == "SHOW" && words.size() >= 2 &&
                 words[1] == "DATABASES") {
        for (const auto &name : database_.ShowDatabases()) output << name << '\n';
      } else if (words[0] == "USE") {
        database_.UseDatabase(tokens[1].text);
        output << "Using database " << database_.CurrentDatabase() << ".\n";
      } else if (words[0] == "DROP" && words.size() >= 2 &&
                 words[1] == "DATABASE") {
        database_.DropDatabase(tokens[2].text);
        output << "Database dropped.\n";
      } else if (words[0] == "CREATE" && words.size() >= 2 &&
                 words[1] == "TABLE") {
        HandleCreateTable(tokens, database_);
        output << "Table created.\n";
      } else if (words[0] == "SHOW" && words.size() >= 2 &&
                 words[1] == "TABLES") {
        for (const auto &name : database_.ShowTables()) output << name << '\n';
      } else if (words[0] == "DESCRIBE" || words[0] == "DESC") {
        const auto definition = database_.Describe(tokens[1].text);
        for (const auto &column : definition.schema.Columns())
          output << column.name << '\t'
                 << (column.type == Column::Type::Int32 ? "INT" : "VARCHAR")
                 << '\n';
      } else if (words[0] == "DROP" && words.size() >= 2 &&
                 words[1] == "TABLE") {
        database_.DropTable(tokens[2].text);
        output << "Table dropped.\n";
      } else {
        PrintResult(database_.ExecuteSql(statement), output);
      }
    } catch (const std::exception &error) {
      output << "Error: " << error.what() << '\n';
    }
    statement.clear();
    output << "MiniDB> ";
  }
  database_.Flush();
  return 0;
}

int main(int argc, char **argv) {
  const std::filesystem::path data_directory =
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("data");
  minidb::Shell shell(data_directory);
  return shell.Run(std::cin, std::cout);
}
