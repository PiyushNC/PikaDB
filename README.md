# PikaDB

![alt text](Gemini_Generated_Image_rulom0rulom0rulo.png)


> A compact, persistent SQL database engine and desktop client built in C++20.

PikaDB is an educational single-node relational database system designed to
make database internals understandable without sacrificing a real end-to-end
workflow. It includes durable page storage, a ten-frame buffer pool, a
persistent B+ tree, write-ahead logging, SQL parsing and execution, and a Qt
desktop application with login, database browsing, query editing, and result
tables.

The project is intentionally self-contained: the core engine uses the C++20
standard library and has no external runtime dependency. Qt is optional and
is used only by the desktop application.

## Highlights

- Persistent databases and table schemas
- 4096-byte pages with `int32_t` page IDs
- Exactly ten buffer frames
- O(1) LRU page replacement
- Pin/unpin and dirty-page management
- Disk-backed integer-key B+ tree indexes
- Leaf, internal, and root splitting
- Linked-leaf range scans
- Binary WAL and committed-redo recovery
- SQL shell and interactive Qt desktop application
- Local account registration and login for the desktop client
- Deterministic tests, demo, and benchmark programs

## Supported SQL

### Database and table management

```sql
CREATE DATABASE college;
SHOW DATABASES;
USE college;
DROP DATABASE college;

CREATE TABLE students (
    id INT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    age INT,
    active BOOLEAN
);

SHOW TABLES;
DESCRIBE students;
ALTER TABLE students ADD COLUMN graduated BOOLEAN;
DROP TABLE students;
```

### Data operations

```sql
INSERT INTO students VALUES
    (1, 'Amit', 21, TRUE),
    (2, 'Sara', 22, FALSE);

SELECT * FROM students;
SELECT id, name
FROM students
WHERE age >= 21 AND active = TRUE
ORDER BY id DESC
LIMIT 10;

SELECT COUNT(*) FROM students;

UPDATE students
SET active = FALSE
WHERE id = 1;

DELETE FROM students
WHERE id = 2;
```

The supported column types are `INT`, `VARCHAR(n)`, and `BOOLEAN`. Supported
comparison operators are `=`, `!=`, `<`, `>`, `<=`, and `>=`. Conditions
support `AND`, `OR`, and parentheses.

## Architecture

```text
                 Qt Desktop UI / SQL Shell
                             |
                       SQL frontend
                    Lexer -> Parser / AST
                             |
                         Planner
             sequential scan or B+ tree index scan
                             |
                         Executor
             filter -> projection -> mutation/result
                             |
             Table Heap / Tuple / RID / B+ Tree
                             |
                  Buffer Pool / Disk Manager
                             |
                  Database files + WAL files
```

### Storage

`DiskManager` allocates and persists fixed-size 4096-byte pages. The
`BufferPoolManager` maintains a page-to-frame table, tracks pin counts and
dirty pages, and evicts frames using an O(1) LRU replacer. The educational
table heap stores tuples with `RID` values composed of a page ID and slot.

### Indexing

The persistent B+ tree stores `int32_t` keys and tuple RIDs. It supports exact
lookups, insertion, leaf splitting, internal splitting, root splitting,
linked leaves, range scans, and reopening from disk. The planner selects an
index scan when a suitable integer predicate is available; otherwise it uses a
sequential scan.

### Recovery

Mutations are recorded in a binary write-ahead log. WAL records include
`BEGIN`, `INSERT`, `UPDATE`, `COMMIT`, and `ABORT`. The buffer pool flushes WAL
records before dependent dirty data pages, and startup recovery replays
committed changes from complete log records.

### Catalog and persistence

Database and table metadata are persisted on disk. A GUI database created from
PikaDB is stored below:

```text
gui-data/
├── users.db
└── databases/
    └── <database-name>/
        ├── catalog.meta
        ├── <table>.db
        ├── <table>.meta
        └── <table>.wal
```

The SQL shell accepts a data-directory argument and stores databases below
that directory.

## Desktop application

The Qt client provides:

- Local registration and login
- Database selection and creation
- Table browser and schema inspection
- Multi-line SQL editor
- Query history
- Example query loading
- Keyboard execution with `Ctrl+Enter`
- Formatted result tables
- Error and status feedback
- PikaDB dark electric-yellow visual theme
- Bundled PikaDB logo

The login system is intended for local educational use. It is not a
production authentication or authorization service.

## Requirements

### Core engine

- C++20 compiler
- CMake 3.20 or newer
- Ninja or another CMake generator
- PowerShell on Windows

### Desktop client

- Qt 6 Widgets
- A compiler compatible with the installed Qt kit

The project was developed and tested on Windows with Qt 6 MinGW.

## Build and run

### One-command startup on Windows

From PowerShell:

```powershell
Set-Location C:\path\to\mini_database_engine
.\start_minidb.ps1
```

The launcher configures CMake, builds the project, deploys Qt runtime files
when Qt is available, and starts the GUI. Without Qt, it builds and starts the
SQL shell.

If script execution is restricted:

```powershell
powershell -ExecutionPolicy Bypass -File .\start_minidb.ps1
```

### Core build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

Run the shell with a persistent data directory:

```powershell
.\build\minidb_shell.exe .\data
```

Run the demo:

```powershell
.\build\minidb_demo.exe
```

Run the benchmark:

```powershell
.\build\minidb_benchmark.exe
```

### Qt build

```powershell
cmake -S . -B build-qt-kit -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=C:\Qt\Tools\mingw1310_64\bin\g++.exe `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.9.1\mingw_64

cmake --build build-qt-kit --parallel 2

C:\Qt\6.9.1\mingw_64\bin\windeployqt.exe `
  --release `
  --no-translations `
  .\build-qt-kit\minidb_gui.exe

.\build-qt-kit\minidb_gui.exe
```

`windeployqt` copies the Qt DLLs and platform plugins required to run the
desktop application on a Windows machine without Qt installed.

## Testing

Run the complete regression suite with:

```powershell
ctest --test-dir build --output-on-failure
```

The tests cover:

- Page and disk persistence
- Buffer-pool pinning, eviction, and ten-frame behavior
- LRU replacement
- Tuple and table storage
- B+ tree search, splits, persistence, and range scans
- Randomized B+ tree checks
- WAL persistence and crash recovery
- SQL lexer, parser, planner, and executor
- Catalog lifecycle and restart persistence
- DDL, DML, ordering, limits, counting, and BOOLEAN values

## Project layout

```text
include/minidb/       Public engine interfaces
src/                  Core storage, SQL, catalog, planner, and execution code
gui/                  Qt desktop application
shell/                Interactive SQL shell
tests/                Regression and integration tests
demo/                 Small feature demonstration
benchmarks/           Deterministic performance smoke benchmark
start_minidb.ps1      One-command Windows launcher
```

## Design scope

PikaDB prioritizes clarity and persistence over production-scale performance.
The current implementation does not include:

- B+ tree deletion, merging, or safe primary-key reuse after tombstoning
- Multiple tuples per page
- Joins
- General aggregation, `GROUP BY`, or `HAVING`
- Subqueries, views, or stored procedures
- SQL transaction commands and full MVCC
- Network clients, replication, or concurrent multi-process access
- Production authentication and authorization

These limitations are explicit design boundaries rather than hidden
fallbacks. The existing storage, indexing, recovery, SQL, and UI layers remain
independently testable.

## License

Add the project license here before publishing to GitHub.
