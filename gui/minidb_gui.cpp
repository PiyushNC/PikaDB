#include "minidb/minidb.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <fstream>
#include <sstream>

void ApplyPikaTheme(QApplication &app) {
  app.setStyleSheet(R"qss(
    * {
      color: #F5F5F7;
      font-family: "Segoe UI", "Arial";
      font-size: 10pt;
    }
    QMainWindow, QWidget {
      background-color: #181825;
    }
    QMainWindow {
      border: 1px solid #3A3A52;
    }
    QLabel#brandTitle {
      color: #FFE033;
      font-size: 22pt;
      font-weight: 800;
    }
    QLabel#brandSubtitle, QLabel#statusLabel {
      color: #B8B8C7;
    }
    QGroupBox {
      background-color: #252538;
      border: 1px solid #3A3A52;
      border-radius: 10px;
      margin-top: 12px;
      padding: 12px;
      font-weight: 700;
    }
    QLineEdit, QComboBox, QTextEdit, QListWidget, QTableWidget {
      background-color: #252538;
      color: #F5F5F7;
      border: 1px solid #4A4A63;
      border-radius: 8px;
      selection-background-color: #FFE033;
      selection-color: #181825;
    }
    QLineEdit, QComboBox {
      min-height: 30px;
      padding: 2px 8px;
    }
    QTextEdit {
      border: 2px solid #3D3D58;
      border-radius: 10px;
      padding: 10px;
      selection-background-color: #FFE033;
      selection-color: #181825;
    }
    QTextEdit:focus, QLineEdit:focus, QComboBox:focus {
      border: 2px solid #FFE033;
    }
    QPushButton {
      background-color: #FFE033;
      color: #181825;
      border: 1px solid #D9B900;
      border-radius: 8px;
      padding: 8px 16px;
      min-height: 18px;
      font-weight: 700;
    }
    QPushButton:hover {
      background-color: #FFF27A;
      border-color: #FFE033;
    }
    QPushButton:pressed {
      background-color: #FF5733;
      color: #181825;
      border-color: #FF5733;
    }
    QPushButton:disabled {
      background-color: #555568;
      color: #A5A5B3;
      border-color: #555568;
    }
    QTableWidget {
      gridline-color: #3B3B51;
      alternate-background-color: #202033;
      selection-background-color: #FFE033;
      selection-color: #181825;
    }
    QHeaderView::section {
      background-color: #252538;
      color: #FFE033;
      border: 0;
      border-bottom: 2px solid #FFE033;
      padding: 7px;
      font-weight: 700;
    }
    QTableCornerButton::section {
      background-color: #252538;
      border: 0;
    }
    QListWidget::item {
      padding: 7px;
      border-radius: 5px;
    }
    QListWidget::item:hover {
      background-color: #34344D;
    }
    QListWidget::item:selected {
      background-color: #FFE033;
      color: #181825;
      font-weight: 700;
    }
    QComboBox QAbstractItemView {
      background-color: #252538;
      color: #F5F5F7;
      selection-background-color: #FFE033;
      selection-color: #181825;
    }
    QSplitter::handle {
      background-color: #3A3A52;
    }
    QScrollBar:vertical, QScrollBar:horizontal {
      background-color: #181825;
      border: 0;
      margin: 0;
    }
    QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
      background-color: #4A4A63;
      border-radius: 5px;
      min-height: 24px;
      min-width: 24px;
    }
    QScrollBar::handle:hover {
      background-color: #FFE033;
    }
    QToolTip {
      background-color: #252538;
      color: #F5F5F7;
      border: 1px solid #FFE033;
      padding: 5px;
    }
  )qss");
}

namespace {
QString HashPassword(const QString &password) {
  return QString::fromLatin1(
      QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString ValueText(const minidb::Value &value) {
  return std::visit([](const auto &item) {
    using T = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<T, std::string>)
      return QString::fromStdString(item);
    else if constexpr (std::is_same_v<T, bool>)
      return item ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
    else
      return QString::fromStdString(std::to_string(item));
  }, value);
}

class MiniDbWindow final : public QMainWindow {
 public:
  explicit MiniDbWindow(const QString &root) : root_(root), database_(root.toStdString() + "/databases") {
    setWindowTitle("PikaDB");
    resize(1200, 760);
    BuildLogin();
  }

 private:
  void BuildLogin() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *logo = new QLabel;
    logo->setAlignment(Qt::AlignCenter);
    const auto logo_path = QDir(QCoreApplication::applicationDirPath())
                               .filePath("Gemini_Generated_Image_rulom0rulom0rulo.png");
    QPixmap logo_pixmap(logo_path);
    if (!logo_pixmap.isNull()) {
      logo->setPixmap(logo_pixmap.scaled(220, 220, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
    } else {
      logo->setText("PikaDB");
      logo->setObjectName("brandTitle");
    }
    auto *title = new QLabel("PikaDB");
    title->setObjectName("brandTitle");
    title->setAlignment(Qt::AlignCenter);
    auto *subtitle = new QLabel("SQL database workspace");
    subtitle->setObjectName("brandSubtitle");
    subtitle->setAlignment(Qt::AlignCenter);
    auto *form = new QFormLayout;
    username_ = new QLineEdit;
    username_->setPlaceholderText("Enter a username");
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText("Enter a password");
    form->addRow("Username", username_);
    form->addRow("Password", password_);
    auto *login = new QPushButton("Log in");
    auto *register_button = new QPushButton("Create account");
    login_status_ = new QLabel;
    login_status_->setWordWrap(true);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(login);
    buttons->addWidget(register_button);
    layout->addStretch();
    layout->addWidget(logo);
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(login_status_);
    layout->addStretch();
    page->setMaximumWidth(520);
    auto *container = new QWidget;
    auto *container_layout = new QHBoxLayout(container);
    container_layout->addStretch();
    container_layout->addWidget(page);
    container_layout->addStretch();
    setCentralWidget(container);
    connect(login, &QPushButton::clicked, this, [this] { Login(); });
    connect(register_button, &QPushButton::clicked, this, [this] { Register(); });
  }

  QString UserFile() const { return root_ + "/users.db"; }

  bool FindUser(const QString &user, QString *hash = nullptr) const {
    std::ifstream file(UserFile().toStdString());
    std::string line;
    while (std::getline(file, line)) {
      const auto separator = line.find('|');
      if (separator == std::string::npos) continue;
      if (QString::fromStdString(line.substr(0, separator)) == user) {
        if (hash) *hash = QString::fromStdString(line.substr(separator + 1));
        return true;
      }
    }
    return false;
  }

  void Register() {
    const auto user = username_->text().trimmed();
    const auto password = password_->text();
    if (user.isEmpty()) {
      username_->setFocus();
      return Error("Please enter a username.");
    }
    if (password.isEmpty()) {
      password_->setFocus();
      return Error("Please enter a password.");
    }
    if (FindUser(user)) return Error("That username already exists.");
    QDir().mkpath(root_);
    std::ofstream file(UserFile().toStdString(), std::ios::app);
    if (!file) return Error("Could not open the local user database.");
    file << user.toStdString() << '|' << HashPassword(password).toStdString() << '\n';
    if (!file) return Error("Could not save the account.");
    password_->clear();
    Status("Account created. You can now log in.");
  }

  void Login() {
    QString expected;
    if (!FindUser(username_->text().trimmed(), &expected) ||
        expected != HashPassword(password_->text())) {
      return Error("Invalid username or password.");
    }
    BuildWorkspace();
  }

  static QString FirstWord(const QString &sql) {
    return sql.trimmed().section(QRegularExpression("\\s+"), 0, 0).toUpper();
  }

  void BuildWorkspace() {
    auto *central = new QWidget;
    auto *root_layout = new QVBoxLayout(central);
    auto *top = new QHBoxLayout;
    database_combo_ = new QComboBox;
    auto *refresh = new QPushButton("Refresh");
    auto *new_database = new QPushButton("New database");
    top->addWidget(new QLabel("Database:"));
    top->addWidget(database_combo_, 1);
    top->addWidget(new_database);
    top->addWidget(refresh);
    root_layout->addLayout(top);

    auto *splitter = new QSplitter;
    tables_ = new QListWidget;
    schema_ = new QTableWidget;
    schema_->setColumnCount(3);
    schema_->setHorizontalHeaderLabels({"Column", "Type", "Constraints"});
    auto *left = new QWidget;
    auto *left_layout = new QVBoxLayout(left);
    left_layout->addWidget(new QLabel("<b>Tables</b>"));
    left_layout->addWidget(tables_);
    splitter->addWidget(left);

    auto *right = new QWidget;
    auto *right_layout = new QVBoxLayout(right);
    sql_ = new QTextEdit;
    sql_->setPlaceholderText("Enter SQL, for example:\nSELECT * FROM students;");
    sql_->setPlainText("SHOW DATABASES;");
    auto *actions = new QHBoxLayout;
    auto *execute = new QPushButton("Execute SQL");
    auto *clear = new QPushButton("Clear editor");
    auto *example = new QPushButton("Load example");
    actions->addWidget(execute);
    actions->addWidget(clear);
    actions->addWidget(example);
    actions->addStretch();
    history_ = new QComboBox;
    history_->setPlaceholderText("Query history");
    results_ = new QTableWidget;
    status_ = new QLabel;
    status_->setWordWrap(true);
    right_layout->addWidget(new QLabel("<b>SQL editor</b>"));
    right_layout->addWidget(sql_, 1);
    right_layout->addLayout(actions);
    right_layout->addWidget(history_);
    right_layout->addWidget(new QLabel("<b>Results</b>"));
    right_layout->addWidget(results_, 2);
    right_layout->addWidget(status_);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    root_layout->addWidget(splitter, 1);
    setCentralWidget(central);

    connect(execute, &QPushButton::clicked, this, [this] { ExecuteSql(); });
    connect(clear, &QPushButton::clicked, this, [this] { sql_->clear(); sql_->setFocus(); });
    connect(example, &QPushButton::clicked, this, [this] {
      sql_->setPlainText(
          "CREATE DATABASE bookstore;\n"
          "USE bookstore;\n"
          "CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR(100) NOT NULL, price INT);\n"
          "INSERT INTO books VALUES (1, 'The Hobbit', 15), (2, 'Dune', 20);\n"
          "SELECT * FROM books ORDER BY id;");
      sql_->setFocus();
    });
    connect(history_, &QComboBox::activated, this, [this](int index) {
      if (index >= 0) sql_->setPlainText(history_->itemText(index));
      sql_->setFocus();
    });
    new QShortcut(QKeySequence("Ctrl+Return"), sql_, [this] { ExecuteSql(); });
    new QShortcut(QKeySequence("Ctrl+Enter"), sql_, [this] { ExecuteSql(); });
    connect(refresh, &QPushButton::clicked, this, [this] { RefreshDatabases(); });
    connect(new_database, &QPushButton::clicked, this, [this] {
      const auto name = QInputDialog::getText(this, "Create database", "Database name:");
      if (!name.isEmpty()) RunCommand("CREATE DATABASE " + name + ";");
    });
    connect(database_combo_, &QComboBox::currentTextChanged, this,
            [this](const QString &name) { if (!name.isEmpty()) RunCommand("USE " + name + ";"); });
    connect(tables_, &QListWidget::currentTextChanged, this,
            [this](const QString &name) { if (!name.isEmpty()) Describe(name); });
    RefreshDatabases();
  }

  void RefreshDatabases() {
    try {
      if (!database_combo_) return;
      const auto names = database_.ShowDatabases();
      database_combo_->blockSignals(true);
      database_combo_->clear();
      for (const auto &name : names) database_combo_->addItem(QString::fromStdString(name));
      if (database_.HasDatabase())
        database_combo_->setCurrentText(QString::fromStdString(database_.CurrentDatabase()));
      database_combo_->blockSignals(false);
      RefreshTables();
      Status(database_.HasDatabase()
          ? "Connected to " + QString::fromStdString(database_.CurrentDatabase())
          : "Create or select a database to begin.");
    } catch (const std::exception &error) {
      Error(QString::fromStdString(error.what()));
    }
  }

  void RefreshTables() {
    if (!tables_) return;
    tables_->clear();
    if (!database_.HasDatabase()) return;
    for (const auto &name : database_.ShowTables())
      tables_->addItem(QString::fromStdString(name));
  }

  void Describe(const QString &name) {
    try {
      const auto definition = database_.Describe(name.toStdString());
      schema_->setRowCount(0);
      for (const auto &column : definition.schema.Columns()) {
        const int row = schema_->rowCount();
        schema_->insertRow(row);
        schema_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(column.name)));
        schema_->setItem(row, 1, new QTableWidgetItem(
            column.type == minidb::Column::Type::Int32 ? "INT" :
            column.type == minidb::Column::Type::Boolean ? "BOOLEAN" : "VARCHAR"));
        QString constraints;
        if (column.primary_key) constraints += "PRIMARY KEY ";
        if (column.not_null) constraints += "NOT NULL";
        schema_->setItem(row, 2, new QTableWidgetItem(constraints.trimmed()));
      }
    } catch (const std::exception &error) { Error(QString::fromStdString(error.what())); }
  }

  void ExecuteSql() { RunCommand(sql_->toPlainText()); }

  static std::string CleanName(QString value) {
    return value.trimmed().remove(';').toStdString();
  }

  void CreateTableFromTokens(const std::vector<minidb::Token> &tokens) {
    std::size_t p = 0;
    auto expect = [&](minidb::TokenKind kind, const char *message) {
      if (p >= tokens.size() || tokens[p].kind != kind)
        throw std::runtime_error(message);
      ++p;
    };
    if (tokens.size() < 5 ||
        QString::fromStdString(tokens[p++].text).toUpper() != "CREATE" ||
        QString::fromStdString(tokens[p++].text).toUpper() != "TABLE")
      throw std::runtime_error("invalid CREATE TABLE statement");
    const std::string name = tokens[p++].text;
    expect(minidb::TokenKind::LParen, "CREATE TABLE requires '('");
    std::vector<minidb::Column> columns;
    while (p < tokens.size() && tokens[p].kind != minidb::TokenKind::RParen) {
      if (tokens[p].kind != minidb::TokenKind::Identifier)
        throw std::runtime_error("expected column name");
      const std::string column_name = tokens[p++].text;
      if (p >= tokens.size()) throw std::runtime_error("missing column type");
      const auto type = QString::fromStdString(tokens[p++].text).toUpper();
      if (type != "INT" && type != "VARCHAR" && type != "BOOLEAN")
        throw std::runtime_error("supported types are INT, VARCHAR, and BOOLEAN");
      minidb::Column column{column_name,
          type == "INT" ? minidb::Column::Type::Int32 :
          type == "BOOLEAN" ? minidb::Column::Type::Boolean : minidb::Column::Type::Varchar};
      if (p < tokens.size() && tokens[p].kind == minidb::TokenKind::LParen) {
        ++p;
        if (p >= tokens.size() || tokens[p].kind != minidb::TokenKind::Integer)
          throw std::runtime_error("VARCHAR length is invalid");
        ++p;
        expect(minidb::TokenKind::RParen, "VARCHAR length requires ')'");
      }
      while (p < tokens.size() && tokens[p].kind != minidb::TokenKind::Comma &&
             tokens[p].kind != minidb::TokenKind::RParen) {
        const auto constraint = QString::fromStdString(tokens[p++].text).toUpper();
        if (constraint == "PRIMARY") {
          if (p >= tokens.size() ||
              QString::fromStdString(tokens[p++].text).toUpper() != "KEY")
            throw std::runtime_error("PRIMARY requires KEY");
          column.primary_key = true;
        } else if (constraint == "NOT") {
          if (p >= tokens.size() ||
              QString::fromStdString(tokens[p++].text).toUpper() != "NULL")
            throw std::runtime_error("NOT requires NULL");
          column.not_null = true;
        } else {
          throw std::runtime_error("unsupported column constraint");
        }
      }
      columns.push_back(column);
      if (p < tokens.size() && tokens[p].kind == minidb::TokenKind::Comma) ++p;
    }
    expect(minidb::TokenKind::RParen, "CREATE TABLE requires ')'");
    database_.CreateTable({name, minidb::Schema(std::move(columns))});
    Status("Table created.");
  }

  void RunCommand(const QString &command) {
    try {
      const auto sql = command.trimmed();
      if (sql.isEmpty()) return;
      if (!history_ || history_->findText(sql) < 0) {
        if (history_) {
          history_->insertItem(0, sql);
          history_->setCurrentIndex(0);
        }
      }
      const auto result = database_.ExecuteSql(sql.toStdString());
      Display(result);
      RefreshTables();
      RefreshDatabases();
    } catch (const std::exception &error) { Error(QString::fromStdString(error.what())); }
  }

  void ShowList(const std::vector<std::string> &items, const QString &label) {
    results_->clear();
    results_->setColumnCount(1);
    results_->setRowCount(static_cast<int>(items.size()));
    results_->setHorizontalHeaderLabels({label});
    for (int i = 0; i < static_cast<int>(items.size()); ++i)
      results_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(items[i])));
    Status(QString::number(items.size()) + " " + label + ".");
  }

  void Display(const minidb::QueryResult &result) {
    results_->clear();
    results_->setColumnCount(static_cast<int>(result.columns.size()));
    results_->setRowCount(static_cast<int>(result.rows.size()));
    QStringList headers;
    for (const auto &column : result.columns) headers << QString::fromStdString(column);
    results_->setHorizontalHeaderLabels(headers);
    for (int row = 0; row < static_cast<int>(result.rows.size()); ++row)
      for (int column = 0; column < static_cast<int>(result.rows[row].size()); ++column)
        results_->setItem(row, column, new QTableWidgetItem(ValueText(result.rows[row][column])));
    results_->resizeColumnsToContents();
    results_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Status(QString::number(result.rows.size()) + " row(s).");
  }

  void Status(const QString &message) {
    if (status_) status_->setText(message);
    else if (login_status_) login_status_->setText(message);
  }
  void Error(const QString &message) { if (status_) status_->setText("Error: " + message); else QMessageBox::critical(this, "MiniDB", message); }

  QString root_;
  minidb::Database database_;
  QLineEdit *username_{nullptr};
  QLineEdit *password_{nullptr};
  QComboBox *database_combo_{nullptr};
  QComboBox *history_{nullptr};
  QListWidget *tables_{nullptr};
  QTableWidget *schema_{nullptr};
  QTableWidget *results_{nullptr};
  QTextEdit *sql_{nullptr};
  QLabel *status_{nullptr};
  QLabel *login_status_{nullptr};
};
}  // namespace

int main(int argc, char **argv) {
  try {
    QApplication app(argc, argv);
    ApplyPikaTheme(app);
    MiniDbWindow window(QDir::currentPath() + "/gui-data");
    window.show();
    return app.exec();
  } catch (const std::exception &error) {
    QMessageBox::critical(nullptr, "MiniDB startup error", error.what());
    return 1;
  }
}
