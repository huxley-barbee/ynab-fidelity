# fynab

A local CLI tool for importing Fidelity CSV exports into an encrypted SQLite database, exporting clean CSVs for YNAB, and diffing YNAB exports against your local records.

Your financial data never leaves your machine.

---

## Requirements

- macOS (arm64 or x86_64)
- Apple Clang 15+ (`xcode-select --install`)
- Homebrew
- SQLCipher (`brew install sqlcipher`)

## Build

```sh
brew install sqlcipher
make
```

The binary is `./fynab`.

## Usage

```sh
./fynab [path/to/database.db]
```

If no path is given, `fynab.db` in the current directory is used. On first run, you will be prompted to create and password-protect the database.

### Menu options

| Option | Description |
|--------|-------------|
| 1 — Import Fidelity CSV | Parse a Fidelity CSV export, skip duplicates, store new records |
| 2 — List imports | Show all import sessions with timestamps and row counts |
| 3 — List records | Show records for a specific import, or all records |
| 4 — Export to YNAB CSV | Write records as a YNAB-compatible CSV (Date/Payee/Memo/Outflow/Inflow) |
| 5 — Diff against YNAB export | Compare DB records against a YNAB register export to find mismatches |
| 6 — Delete an import | Remove an import session and all its records |
| 7 — Delete a single record | Remove one record by its row ID |

### Typical monthly workflow

1. Download your Fidelity CSV (Activity & Orders → Export)
2. Run `./fynab` → option 1 → point at the CSV
3. Option 4 → export to YNAB CSV
4. Import that CSV into YNAB (File-based import on the account)
5. At month-end, export your YNAB register and run option 5 to verify everything matches

### Duplicate detection

Records are deduplicated on `(date, amount, description)`. Importing the same CSV twice, or overlapping date ranges across multiple exports, is safe — already-stored records are silently skipped.

### Database

The database is a standard SQLite file encrypted with SQLCipher (AES-256). The password is required every time the program runs. There is no password recovery; keep it safe.

To back up: copy `fynab.db` anywhere. It is self-contained and encrypted.

## Schema

```sql
imports (id, created_at, source_file, record_count)
records (id, import_id, date, description, amount, type, symbol)
```

`records.import_id` references `imports.id` with `ON DELETE CASCADE`, so deleting an import removes all its records.

## Building on Intel Mac

```sh
BREW_PREFIX=/usr/local make
```

## License

MIT
