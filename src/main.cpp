#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <termios.h>
#include <unistd.h>
#include <sqlite3.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Terminal helpers
// ---------------------------------------------------------------------------

static std::string readPassword(const std::string& prompt) {
    std::cerr << prompt << std::flush;
    termios old{}, neo{};
    tcgetattr(STDIN_FILENO, &old);
    neo = old;
    neo.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
    tcsetattr(STDIN_FILENO, TCSANOW, &neo);
    std::string pw;
    std::getline(std::cin, pw);
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    std::cerr << "\n";
    return pw;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool confirm(const std::string& prompt) {
    std::cout << prompt << " [y/N] " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return toLower(trim(line)) == "y";
}

// ---------------------------------------------------------------------------
// CSV parsing
// ---------------------------------------------------------------------------

struct CsvRow {
    std::vector<std::string> fields;
};

static std::vector<std::string> parseCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool inQ = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (inQ && i + 1 < line.size() && line[i+1] == '"') {
                cur += '"'; ++i;
            } else {
                inQ = !inQ;
            }
        } else if (c == ',' && !inQ) {
            fields.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    fields.push_back(trim(cur));
    return fields;
}

// ---------------------------------------------------------------------------
// Fidelity CSV → Record
// ---------------------------------------------------------------------------

struct Record {
    int64_t id = 0;          // db rowid (0 = not yet stored)
    int64_t import_id = 0;
    std::string date;         // YYYY-MM-DD
    std::string description;
    double amount = 0.0;      // positive = inflow, negative = outflow
    std::string type;
    std::string symbol;
};

static std::string normalizeDate(const std::string& raw) {
    // Accept MM/DD/YYYY or YYYY-MM-DD
    if (raw.size() == 10 && raw[4] == '-') return raw;
    if (raw.size() >= 8) {
        // try M/D/YYYY or MM/DD/YYYY
        std::istringstream ss(raw);
        int m, d, y;
        char sep;
        if (ss >> m >> sep >> d >> sep >> y) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
            return std::string(buf);
        }
    }
    return raw;
}

static double parseAmount(const std::string& s) {
    std::string clean;
    for (char c : s)
        if (c != '$' && c != ',' && c != ' ') clean += c;
    try { return std::stod(clean); } catch (...) { return 0.0; }
}

// Returns records parsed from a Fidelity CSV file.
// Fidelity exports have a variable number of header rows before the column row.
static std::vector<Record> parseFidelityCSV(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open file: " + path);

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);

    // Find header row: must contain "Date" and "Amount"
    int headerIdx = -1;
    std::map<std::string, int> colIdx;

    for (int i = 0; i < (int)lines.size(); ++i) {
        auto fields = parseCsvLine(lines[i]);
        bool hasDate = false, hasAmt = false;
        for (auto& fld : fields) {
            std::string lo = toLower(fld);
            if (lo == "date") hasDate = true;
            if (lo.find("amount") != std::string::npos) hasAmt = true;
        }
        if (hasDate && hasAmt) {
            headerIdx = i;
            for (int j = 0; j < (int)fields.size(); ++j)
                colIdx[toLower(fields[j])] = j;
            break;
        }
    }

    if (headerIdx < 0)
        throw std::runtime_error("Could not find header row in: " + path);

    auto col = [&](const std::string& name, int def = -1) -> int {
        auto it = colIdx.find(name);
        return it != colIdx.end() ? it->second : def;
    };

    int cDate = col("date");
    // Fidelity uses "amount ($)" or "amount"
    int cAmt = -1;
    for (auto& [k, v] : colIdx)
        if (k.find("amount") != std::string::npos) { cAmt = v; break; }
    int cDesc = col("description", col("name", -1));
    int cType = col("type", col("transaction type", -1));
    int cSym  = col("symbol", -1);

    if (cDate < 0 || cAmt < 0)
        throw std::runtime_error("Missing required columns in: " + path);

    std::vector<Record> records;
    for (int i = headerIdx + 1; i < (int)lines.size(); ++i) {
        if (trim(lines[i]).empty()) continue;
        auto flds = parseCsvLine(lines[i]);
        if ((int)flds.size() <= std::max(cDate, cAmt)) continue;

        std::string dateRaw = cDate < (int)flds.size() ? flds[cDate] : "";
        std::string amtRaw  = cAmt  < (int)flds.size() ? flds[cAmt]  : "";
        if (dateRaw.empty() || amtRaw.empty()) continue;

        Record r;
        r.date        = normalizeDate(dateRaw);
        r.amount      = parseAmount(amtRaw);
        r.description = cDesc >= 0 && cDesc < (int)flds.size() ? flds[cDesc] : "";
        r.type        = cType >= 0 && cType < (int)flds.size() ? flds[cType] : "";
        r.symbol      = cSym  >= 0 && cSym  < (int)flds.size() ? flds[cSym]  : "";
        records.push_back(r);
    }
    return records;
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

class DB {
public:
    sqlite3* db = nullptr;

    ~DB() { if (db) sqlite3_close(db); }

    void open(const std::string& path, const std::string& password) {
        int rc = sqlite3_open(path.c_str(), &db);
        if (rc != SQLITE_OK)
            throw std::runtime_error(std::string("Cannot open db: ") + sqlite3_errmsg(db));

        // Set encryption key via SQLCipher
        std::string keyPragma = "PRAGMA key = '" + escapeSql(password) + "';";
        exec(keyPragma);

        // Verify we can read (wrong password → this fails)
        exec("SELECT count(*) FROM sqlite_master;");
    }

    void create(const std::string& path, const std::string& password) {
        open(path, password);
        exec(R"(
            CREATE TABLE IF NOT EXISTS imports (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at  TEXT NOT NULL,
                source_file TEXT NOT NULL,
                record_count INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS records (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                import_id   INTEGER NOT NULL REFERENCES imports(id) ON DELETE CASCADE,
                date        TEXT NOT NULL,
                description TEXT NOT NULL,
                amount      REAL NOT NULL,
                type        TEXT,
                symbol      TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_records_dedup
                ON records(date, amount, description);
        )");
    }

    void exec(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error("SQL error: " + msg + "\nSQL: " + sql.substr(0,120));
        }
    }

    struct Stmt {
        sqlite3_stmt* s = nullptr;
        ~Stmt() { if (s) sqlite3_finalize(s); }
    };

    Stmt prepare(const std::string& sql) {
        Stmt st;
        int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &st.s, nullptr);
        if (rc != SQLITE_OK)
            throw std::runtime_error(std::string("Prepare error: ") + sqlite3_errmsg(db));
        return st;
    }

    // Returns existing (date, amount, description) tuples to detect duplicates
    std::set<std::tuple<std::string,double,std::string>> existingKeys() {
        std::set<std::tuple<std::string,double,std::string>> keys;
        auto st = prepare("SELECT date, amount, description FROM records;");
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            std::string d = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 0));
            double a = sqlite3_column_double(st.s, 1);
            std::string desc = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            keys.emplace(d, a, desc);
        }
        return keys;
    }

    int64_t insertImport(const std::string& sourceFile, int count) {
        auto st = prepare(
            "INSERT INTO imports(created_at, source_file, record_count) VALUES(datetime('now'), ?, ?);");
        sqlite3_bind_text(st.s, 1, sourceFile.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st.s, 2, count);
        sqlite3_step(st.s);
        return sqlite3_last_insert_rowid(db);
    }

    void insertRecord(const Record& r) {
        auto st = prepare(
            "INSERT INTO records(import_id, date, description, amount, type, symbol)"
            " VALUES(?, ?, ?, ?, ?, ?);");
        sqlite3_bind_int64(st.s, 1, r.import_id);
        sqlite3_bind_text(st.s, 2, r.date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.s, 3, r.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st.s, 4, r.amount);
        sqlite3_bind_text(st.s, 5, r.type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.s, 6, r.symbol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st.s);
    }

    struct ImportMeta {
        int64_t id;
        std::string created_at;
        std::string source_file;
        int record_count;
    };

    std::vector<ImportMeta> listImports() {
        std::vector<ImportMeta> out;
        auto st = prepare("SELECT id, created_at, source_file, record_count FROM imports ORDER BY id;");
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            ImportMeta m;
            m.id           = sqlite3_column_int64(st.s, 0);
            m.created_at   = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 1));
            m.source_file  = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            m.record_count = sqlite3_column_int(st.s, 3);
            out.push_back(m);
        }
        return out;
    }

    std::vector<Record> recordsForImport(int64_t import_id) {
        std::vector<Record> out;
        auto st = prepare(
            "SELECT id, import_id, date, description, amount, type, symbol"
            " FROM records WHERE import_id = ? ORDER BY date, id;");
        sqlite3_bind_int64(st.s, 1, import_id);
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            Record r;
            r.id          = sqlite3_column_int64(st.s, 0);
            r.import_id   = sqlite3_column_int64(st.s, 1);
            r.date        = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            r.description = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 3));
            r.amount      = sqlite3_column_double(st.s, 4);
            r.type        = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 5) ? sqlite3_column_text(st.s, 5) : (const unsigned char*)"");
            r.symbol      = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 6) ? sqlite3_column_text(st.s, 6) : (const unsigned char*)"");
            out.push_back(r);
        }
        return out;
    }

    std::vector<Record> allRecords() {
        std::vector<Record> out;
        auto st = prepare(
            "SELECT id, import_id, date, description, amount, type, symbol"
            " FROM records ORDER BY date, id;");
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            Record r;
            r.id          = sqlite3_column_int64(st.s, 0);
            r.import_id   = sqlite3_column_int64(st.s, 1);
            r.date        = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            r.description = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 3));
            r.amount      = sqlite3_column_double(st.s, 4);
            r.type        = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 5) ? sqlite3_column_text(st.s, 5) : (const unsigned char*)"");
            r.symbol      = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 6) ? sqlite3_column_text(st.s, 6) : (const unsigned char*)"");
            out.push_back(r);
        }
        return out;
    }

    void deleteImport(int64_t import_id) {
        // CASCADE deletes records
        auto st = prepare("DELETE FROM imports WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, import_id);
        sqlite3_step(st.s);
    }

    void deleteRecord(int64_t record_id) {
        auto st = prepare("DELETE FROM records WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, record_id);
        sqlite3_step(st.s);
    }

    bool importExists(int64_t id) {
        auto st = prepare("SELECT 1 FROM imports WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, id);
        return sqlite3_step(st.s) == SQLITE_ROW;
    }

    bool recordExists(int64_t id) {
        auto st = prepare("SELECT 1 FROM records WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, id);
        return sqlite3_step(st.s) == SQLITE_ROW;
    }

private:
    static std::string escapeSql(const std::string& s) {
        std::string out;
        for (char c : s) { if (c == '\'') out += '\''; out += c; }
        return out;
    }
};

// ---------------------------------------------------------------------------
// YNAB export
// ---------------------------------------------------------------------------

static std::string ynabDate(const std::string& iso) {
    // YYYY-MM-DD → MM/DD/YYYY
    if (iso.size() == 10 && iso[4] == '-') {
        return iso.substr(5,2) + "/" + iso.substr(8,2) + "/" + iso.substr(0,4);
    }
    return iso;
}

static std::string csvQuote(const std::string& s) {
    if (s.find(',') == std::string::npos && s.find('"') == std::string::npos)
        return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
}

static void exportYNAB(const std::vector<Record>& records, const std::string& outPath) {
    std::ofstream f(outPath);
    if (!f) throw std::runtime_error("Cannot write: " + outPath);
    f << "Date,Payee,Memo,Outflow,Inflow\n";
    for (auto& r : records) {
        std::string outflow = r.amount < 0 ? std::to_string(-r.amount) : "";
        std::string inflow  = r.amount > 0 ? std::to_string(r.amount)  : "";
        // Round to 2dp
        auto fmt = [](double v) -> std::string {
            char buf[32]; snprintf(buf, sizeof(buf), "%.2f", v); return buf;
        };
        outflow = r.amount < 0 ? fmt(-r.amount) : "";
        inflow  = r.amount > 0 ? fmt(r.amount)  : "";
        f << csvQuote(ynabDate(r.date)) << ","
          << csvQuote(r.description)   << ","
          << ","                        // Memo blank
          << outflow << ","
          << inflow  << "\n";
    }
    std::cout << "Exported " << records.size() << " records to: " << outPath << "\n";
}

// ---------------------------------------------------------------------------
// YNAB reconciliation diff
// ---------------------------------------------------------------------------

struct YNABRecord {
    std::string date;
    std::string payee;
    std::string memo;
    double outflow = 0.0;
    double inflow  = 0.0;
    double amount() const { return inflow - outflow; }
};

static std::vector<YNABRecord> parseYNABExport(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open: " + path);

    std::vector<YNABRecord> out;
    std::string line;
    bool header = true;
    std::map<std::string,int> colIdx;

    while (std::getline(f, line)) {
        auto flds = parseCsvLine(line);
        if (header) {
            for (int i = 0; i < (int)flds.size(); ++i)
                colIdx[toLower(trim(flds[i]))] = i;
            header = false;
            continue;
        }
        if (flds.empty()) continue;
        YNABRecord r;
        auto get = [&](const std::string& k) -> std::string {
            auto it = colIdx.find(k);
            return (it != colIdx.end() && it->second < (int)flds.size()) ? flds[it->second] : "";
        };
        r.date    = normalizeDate(get("date"));
        r.payee   = get("payee");
        r.memo    = get("memo");
        r.outflow = parseAmount(get("outflow"));
        r.inflow  = parseAmount(get("inflow"));
        if (!r.date.empty()) out.push_back(r);
    }
    return out;
}

static void runDiff(DB& db, const std::string& ynabPath) {
    auto ynabRecords = parseYNABExport(ynabPath);
    auto dbRecords   = db.allRecords();

    std::cout << "\nDB records:   " << dbRecords.size() << "\n";
    std::cout << "YNAB records: " << ynabRecords.size() << "\n\n";

    // Build match key: date + rounded amount
    // We match on date + amount (to 2dp) since description may differ slightly
    using Key = std::pair<std::string, std::string>;
    auto amtStr = [](double v) { char b[32]; snprintf(b,sizeof(b),"%.2f",v); return std::string(b); };

    std::map<Key, int> ynabCounts;
    for (auto& r : ynabRecords)
        ynabCounts[{r.date, amtStr(r.amount())}]++;

    std::map<Key, int> dbCounts;
    for (auto& r : dbRecords)
        dbCounts[{r.date, amtStr(r.amount)}]++;

    // In DB but not in YNAB
    std::vector<Record> missingFromYNAB;
    std::map<Key,int> dbRemaining = dbCounts;
    std::map<Key,int> ynabRemaining = ynabCounts;

    for (auto& r : dbRecords) {
        Key k = {r.date, amtStr(r.amount)};
        if (ynabRemaining[k] > 0) ynabRemaining[k]--;
        else missingFromYNAB.push_back(r);
    }

    std::vector<YNABRecord> missingFromDB;
    for (auto& r : ynabRecords) {
        Key k = {r.date, amtStr(r.amount())};
        if (dbRemaining[k] > 0) dbRemaining[k]--;
        else missingFromDB.push_back(r);
    }

    if (missingFromYNAB.empty() && missingFromDB.empty()) {
        std::cout << "✓ Perfect match — all records accounted for on both sides.\n\n";
        return;
    }

    if (!missingFromYNAB.empty()) {
        std::cout << "=== In DB but NOT in YNAB (" << missingFromYNAB.size() << ") ===\n";
        std::cout << std::left
                  << std::setw(12) << "Date"
                  << std::setw(10) << "Amount"
                  << "Description\n";
        std::cout << std::string(70, '-') << "\n";
        for (auto& r : missingFromYNAB) {
            char amt[16]; snprintf(amt, sizeof(amt), "%+.2f", r.amount);
            std::cout << std::setw(12) << r.date
                      << std::setw(10) << amt
                      << r.description.substr(0, 48) << "\n";
        }
        std::cout << "\n";
    }

    if (!missingFromDB.empty()) {
        std::cout << "=== In YNAB but NOT in DB (" << missingFromDB.size() << ") ===\n";
        std::cout << std::left
                  << std::setw(12) << "Date"
                  << std::setw(10) << "Amount"
                  << "Payee\n";
        std::cout << std::string(70, '-') << "\n";
        for (auto& r : missingFromDB) {
            char amt[16]; snprintf(amt, sizeof(amt), "%+.2f", r.amount());
            std::cout << std::setw(12) << r.date
                      << std::setw(10) << amt
                      << r.payee.substr(0, 48) << "\n";
        }
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

static void printImports(const std::vector<DB::ImportMeta>& imports) {
    if (imports.empty()) { std::cout << "(no imports)\n"; return; }
    std::cout << std::left
              << std::setw(6)  << "ID"
              << std::setw(22) << "Imported at"
              << std::setw(8)  << "Rows"
              << "Source file\n";
    std::cout << std::string(80, '-') << "\n";
    for (auto& m : imports) {
        std::cout << std::setw(6)  << m.id
                  << std::setw(22) << m.created_at
                  << std::setw(8)  << m.record_count
                  << m.source_file << "\n";
    }
    std::cout << "\n";
}

static void printRecords(const std::vector<Record>& records) {
    if (records.empty()) { std::cout << "(no records)\n"; return; }
    std::cout << std::left
              << std::setw(8)  << "Row ID"
              << std::setw(12) << "Date"
              << std::setw(10) << "Amount"
              << "Description\n";
    std::cout << std::string(80, '-') << "\n";
    for (auto& r : records) {
        char amt[16]; snprintf(amt, sizeof(amt), "%+.2f", r.amount);
        std::cout << std::setw(8)  << r.id
                  << std::setw(12) << r.date
                  << std::setw(10) << amt
                  << r.description.substr(0, 50) << "\n";
    }
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Interactive menu
// ---------------------------------------------------------------------------

static int64_t promptInt(const std::string& prompt) {
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    try { return std::stoll(trim(line)); } catch (...) { return -1; }
}

static std::string promptString(const std::string& prompt) {
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    return trim(line);
}

static void menuImport(DB& db) {
    std::string path = promptString("Path to Fidelity CSV file: ");
    if (path.empty() || !fs::exists(path)) {
        std::cout << "File not found.\n"; return;
    }

    std::vector<Record> parsed;
    try {
        parsed = parseFidelityCSV(path);
    } catch (std::exception& e) {
        std::cout << "Parse error: " << e.what() << "\n"; return;
    }

    std::cout << "Parsed " << parsed.size() << " rows from file.\n";

    // Dedup against DB
    auto existing = db.existingKeys();
    std::vector<Record> newRecords;
    for (auto& r : parsed) {
        auto key = std::make_tuple(r.date, r.amount, r.description);
        if (existing.find(key) == existing.end()) {
            newRecords.push_back(r);
            existing.insert(key); // prevent intra-batch dups too
        }
    }

    int skipped = (int)parsed.size() - (int)newRecords.size();
    std::cout << "Skipping " << skipped << " duplicate(s). "
              << newRecords.size() << " new record(s) to import.\n";

    if (newRecords.empty()) { std::cout << "Nothing to import.\n"; return; }

    if (!confirm("Proceed?")) return;

    db.exec("BEGIN;");
    int64_t imp_id = db.insertImport(fs::absolute(path).string(), (int)newRecords.size());
    for (auto& r : newRecords) {
        r.import_id = imp_id;
        db.insertRecord(r);
    }
    db.exec("COMMIT;");
    std::cout << "Imported " << newRecords.size() << " records (import ID " << imp_id << ").\n";
}

static void menuListImports(DB& db) {
    printImports(db.listImports());
}

static void menuListRecords(DB& db) {
    int64_t id = promptInt("Import ID (0 = all records): ");
    if (id == 0) {
        printRecords(db.allRecords());
    } else {
        if (!db.importExists(id)) { std::cout << "Import not found.\n"; return; }
        printRecords(db.recordsForImport(id));
    }
}

static void menuDeleteImport(DB& db) {
    printImports(db.listImports());
    int64_t id = promptInt("Import ID to delete: ");
    if (!db.importExists(id)) { std::cout << "Import not found.\n"; return; }
    if (!confirm("Delete import " + std::to_string(id) + " and all its records?")) return;
    db.deleteImport(id);
    std::cout << "Deleted.\n";
}

static void menuDeleteRecord(DB& db) {
    int64_t id = promptInt("Record row ID to delete: ");
    if (!db.recordExists(id)) { std::cout << "Record not found.\n"; return; }
    if (!confirm("Delete record " + std::to_string(id) + "?")) return;
    db.deleteRecord(id);
    std::cout << "Deleted.\n";
}

static void menuExport(DB& db) {
    int64_t id = promptInt("Import ID to export (0 = all records): ");
    std::vector<Record> records;
    if (id == 0) {
        records = db.allRecords();
    } else {
        if (!db.importExists(id)) { std::cout << "Import not found.\n"; return; }
        records = db.recordsForImport(id);
    }
    if (records.empty()) { std::cout << "No records to export.\n"; return; }
    std::string outPath = promptString("Output CSV path [ynab_import.csv]: ");
    if (outPath.empty()) outPath = "ynab_import.csv";
    exportYNAB(records, outPath);
}

static void menuDiff(DB& db) {
    std::string path = promptString("Path to YNAB export CSV: ");
    if (path.empty() || !fs::exists(path)) {
        std::cout << "File not found.\n"; return;
    }
    runDiff(db, path);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::string dbPath = "fynab.db";
    if (argc >= 2) dbPath = argv[1];

    std::cout << "fynab — Fidelity → YNAB bridge\n";
    std::cout << "Database: " << fs::absolute(dbPath).string() << "\n\n";

    bool exists = fs::exists(dbPath);
    DB db;

    try {
        if (!exists) {
            std::cout << "Database not found.\n";
            if (!confirm("Create new database at " + dbPath + "?")) return 0;
            std::string pw  = readPassword("New password: ");
            std::string pw2 = readPassword("Confirm password: ");
            if (pw != pw2) { std::cerr << "Passwords do not match.\n"; return 1; }
            db.create(dbPath, pw);
            std::cout << "Database created.\n\n";
        } else {
            std::string pw = readPassword("Password: ");
            try {
                db.open(dbPath, pw);
                // ensure schema exists (in case of old db without tables)
                db.exec(R"(
                    CREATE TABLE IF NOT EXISTS imports (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        created_at TEXT NOT NULL,
                        source_file TEXT NOT NULL,
                        record_count INTEGER NOT NULL DEFAULT 0
                    );
                    CREATE TABLE IF NOT EXISTS records (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        import_id INTEGER NOT NULL REFERENCES imports(id) ON DELETE CASCADE,
                        date TEXT NOT NULL,
                        description TEXT NOT NULL,
                        amount REAL NOT NULL,
                        type TEXT,
                        symbol TEXT
                    );
                    CREATE INDEX IF NOT EXISTS idx_records_dedup
                        ON records(date, amount, description);
                )");
            } catch (std::exception& e) {
                std::cerr << "Could not open database (wrong password?): " << e.what() << "\n";
                return 1;
            }
        }
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    // Main menu loop
    while (true) {
        std::cout << "------------------------------------\n";
        std::cout << " 1) Import Fidelity CSV\n";
        std::cout << " 2) List imports\n";
        std::cout << " 3) List records\n";
        std::cout << " 4) Export to YNAB CSV\n";
        std::cout << " 5) Diff against YNAB export\n";
        std::cout << " 6) Delete an import\n";
        std::cout << " 7) Delete a single record\n";
        std::cout << " 0) Quit\n";
        std::cout << "------------------------------------\n";
        std::cout << "> ";

        std::string choice;
        if (!std::getline(std::cin, choice)) break;
        choice = trim(choice);

        try {
            if (choice == "1") menuImport(db);
            else if (choice == "2") menuListImports(db);
            else if (choice == "3") menuListRecords(db);
            else if (choice == "4") menuExport(db);
            else if (choice == "5") menuDiff(db);
            else if (choice == "6") menuDeleteImport(db);
            else if (choice == "7") menuDeleteRecord(db);
            else if (choice == "0") break;
            else std::cout << "Unknown option.\n";
        } catch (std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
    }

    return 0;
}
