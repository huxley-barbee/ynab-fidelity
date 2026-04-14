# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```sh
# Install dependency (once)
brew install sqlcipher

# Release build → ./fynab
make

# Debug build (ASan + UBSan) → ./fynab_debug
make debug

# Intel Mac
BREW_PREFIX=/usr/local make

make clean
```

There are no tests. The binary is run interactively: `./fynab [path/to/database.db]`.

## Architecture

The entire program is a single file: `src/main.cpp`. There are no other source files.

Key components, in order of appearance:

- **Terminal helpers** — `readPassword`, `trim`, `toLower`, `confirm`
- **CSV parser** — `parseCsvLine` handles quoted fields; `parseFidelityCSV` auto-detects the header row (Fidelity exports have variable preamble rows) and maps flexible column names (`date`/`run date`, `amount`, `action`/`description`, etc.)
- **`Record` struct** — core data type: `id`, `import_id`, `date` (YYYY-MM-DD), `description`, `amount` (positive=inflow), `type`, `symbol`
- **`DB` class** — wraps SQLCipher (`sqlite3.h`); handles open/create, schema init, and all queries. Nested `Stmt` RAII wrapper for prepared statements.
- **Menu functions** — one function per menu option, called from `main()`

## Database schema

```sql
accounts (id, name, start_date)
imports  (id, account_id, created_at, source_file, record_count)
records  (id, import_id, date, description, amount, type, symbol)
```

`records.import_id` → `imports.id` with `ON DELETE CASCADE`. Dedup index on `records(date, amount, description)`.

The database is SQLCipher (AES-256). Password is required on every run; no recovery mechanism.

## OFX export

The tool also exports OFX files for direct YNAB import (in addition to CSV). OFX transactions use `<TRNTYPE>DEBIT`/`CREDIT` based on sign of amount.
