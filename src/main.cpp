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
    if (raw.size() == 10 && raw[4] == '-') return raw;
    if (raw.size() >= 8) {
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

    int headerIdx = -1;
    std::map<std::string, int> colIdx;

    for (int i = 0; i < (int)lines.size(); ++i) {
        auto fields = parseCsvLine(lines[i]);
        bool hasDate = false, hasAmt = false;
        for (auto& fld : fields) {
            std::string lo = toLower(fld);
            if (lo.find("date") != std::string::npos) hasDate = true;
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

    // Fidelity uses "Date", "Run Date", etc.
    int cDate = -1;
    for (auto& [k, v] : colIdx)
        if (k.find("date") != std::string::npos) { cDate = v; break; }
    int cAmt = -1;
    for (auto& [k, v] : colIdx)
        if (k.find("amount") != std::string::npos) { cAmt = v; break; }
    int cDesc = col("action", col("description", col("name", -1)));
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

        std::string keyPragma = "PRAGMA key = '" + escapeSql(password) + "';";
        exec(keyPragma);
        exec("SELECT count(*) FROM sqlite_master;");
    }

    void create(const std::string& path, const std::string& password) {
        open(path, password);
        exec(R"(
            CREATE TABLE IF NOT EXISTS accounts (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                name       TEXT NOT NULL UNIQUE,
                start_date TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS imports (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                account_id   INTEGER NOT NULL REFERENCES accounts(id),
                created_at   TEXT NOT NULL,
                source_file  TEXT NOT NULL,
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

    // ---------------------------------------------------------------------------
    // Accounts
    // ---------------------------------------------------------------------------

    struct AccountMeta {
        int64_t id;
        std::string name;
        std::string start_date;
    };

    std::vector<AccountMeta> listAccounts() {
        std::vector<AccountMeta> out;
        auto st = prepare("SELECT id, name, start_date FROM accounts ORDER BY id;");
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            AccountMeta a;
            a.id         = sqlite3_column_int64(st.s, 0);
            a.name       = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 1));
            a.start_date = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            out.push_back(a);
        }
        return out;
    }

    int64_t createAccount(const std::string& name, const std::string& start_date) {
        auto st = prepare("INSERT INTO accounts(name, start_date) VALUES(?, ?);");
        sqlite3_bind_text(st.s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.s, 2, start_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st.s);
        return sqlite3_last_insert_rowid(db);
    }

    void updateAccountStartDate(int64_t id, const std::string& start_date) {
        auto st = prepare("UPDATE accounts SET start_date = ? WHERE id = ?;");
        sqlite3_bind_text(st.s, 1, start_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.s, 2, id);
        sqlite3_step(st.s);
    }

    std::string accountStartDate(int64_t id) {
        auto st = prepare("SELECT start_date FROM accounts WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, id);
        if (sqlite3_step(st.s) == SQLITE_ROW)
            return reinterpret_cast<const char*>(sqlite3_column_text(st.s, 0));
        return "";
    }

    bool accountExists(int64_t id) {
        auto st = prepare("SELECT 1 FROM accounts WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, id);
        return sqlite3_step(st.s) == SQLITE_ROW;
    }

    void deleteAccount(int64_t id) {
        auto st = prepare("DELETE FROM accounts WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, id);
        sqlite3_step(st.s);
    }

    // ---------------------------------------------------------------------------
    // Imports
    // ---------------------------------------------------------------------------

    struct ImportMeta {
        int64_t id;
        int64_t account_id;
        std::string account_name;
        std::string created_at;
        std::string source_file;
        int record_count;
    };

    std::vector<ImportMeta> listImports(int64_t account_id = 0) {
        std::string sql =
            "SELECT i.id, i.account_id, a.name, i.created_at, i.source_file, i.record_count"
            " FROM imports i JOIN accounts a ON a.id = i.account_id";
        if (account_id > 0) sql += " WHERE i.account_id = " + std::to_string(account_id);
        sql += " ORDER BY i.id;";
        std::vector<ImportMeta> out;
        auto st = prepare(sql);
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            ImportMeta m;
            m.id           = sqlite3_column_int64(st.s, 0);
            m.account_id   = sqlite3_column_int64(st.s, 1);
            m.account_name = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            m.created_at   = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 3));
            m.source_file  = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 4));
            m.record_count = sqlite3_column_int(st.s, 5);
            out.push_back(m);
        }
        return out;
    }

    int64_t insertImport(int64_t account_id, const std::string& sourceFile, int count) {
        auto st = prepare(
            "INSERT INTO imports(account_id, created_at, source_file, record_count)"
            " VALUES(?, datetime('now'), ?, ?);");
        sqlite3_bind_int64(st.s, 1, account_id);
        sqlite3_bind_text(st.s, 2, sourceFile.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st.s, 3, count);
        sqlite3_step(st.s);
        return sqlite3_last_insert_rowid(db);
    }

    bool importExists(int64_t id) {
        auto st = prepare("SELECT 1 FROM imports WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, id);
        return sqlite3_step(st.s) == SQLITE_ROW;
    }

    void deleteImport(int64_t import_id) {
        auto st = prepare("DELETE FROM imports WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, import_id);
        sqlite3_step(st.s);
    }

    // ---------------------------------------------------------------------------
    // Records
    // ---------------------------------------------------------------------------

    // Dedup keys scoped to a single account so the same transaction on two
    // different accounts is not incorrectly skipped.
    std::set<std::tuple<std::string,double,std::string>> existingKeys(int64_t account_id) {
        std::set<std::tuple<std::string,double,std::string>> keys;
        auto st = prepare(
            "SELECT r.date, r.amount, r.description"
            " FROM records r JOIN imports i ON r.import_id = i.id"
            " WHERE i.account_id = ?;");
        sqlite3_bind_int64(st.s, 1, account_id);
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            std::string d    = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 0));
            double a         = sqlite3_column_double(st.s, 1);
            std::string desc = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 2));
            keys.emplace(d, a, desc);
        }
        return keys;
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

    std::vector<Record> recordsForImport(int64_t import_id) {
        std::vector<Record> out;
        auto st = prepare(
            "SELECT id, import_id, date, description, amount, type, symbol"
            " FROM records WHERE import_id = ? ORDER BY date, id;");
        sqlite3_bind_int64(st.s, 1, import_id);
        while (sqlite3_step(st.s) == SQLITE_ROW) out.push_back(rowToRecord(st.s));
        return out;
    }

    std::vector<Record> recordsForAccount(int64_t account_id) {
        std::vector<Record> out;
        auto st = prepare(
            "SELECT r.id, r.import_id, r.date, r.description, r.amount, r.type, r.symbol"
            " FROM records r JOIN imports i ON r.import_id = i.id"
            " WHERE i.account_id = ? ORDER BY r.date, r.id;");
        sqlite3_bind_int64(st.s, 1, account_id);
        while (sqlite3_step(st.s) == SQLITE_ROW) out.push_back(rowToRecord(st.s));
        return out;
    }

    std::vector<Record> allRecords() {
        std::vector<Record> out;
        auto st = prepare(
            "SELECT id, import_id, date, description, amount, type, symbol"
            " FROM records ORDER BY date, id;");
        while (sqlite3_step(st.s) == SQLITE_ROW) out.push_back(rowToRecord(st.s));
        return out;
    }

    bool recordExists(int64_t id) {
        auto st = prepare("SELECT 1 FROM records WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, id);
        return sqlite3_step(st.s) == SQLITE_ROW;
    }

    void deleteRecord(int64_t record_id) {
        auto st = prepare("DELETE FROM records WHERE id = ?;");
        sqlite3_bind_int64(st.s, 1, record_id);
        sqlite3_step(st.s);
    }

private:
    static Record rowToRecord(sqlite3_stmt* s) {
        Record r;
        r.id          = sqlite3_column_int64(s, 0);
        r.import_id   = sqlite3_column_int64(s, 1);
        r.date        = reinterpret_cast<const char*>(sqlite3_column_text(s, 2));
        r.description = reinterpret_cast<const char*>(sqlite3_column_text(s, 3));
        r.amount      = sqlite3_column_double(s, 4);
        r.type        = reinterpret_cast<const char*>(sqlite3_column_text(s, 5) ? sqlite3_column_text(s, 5) : (const unsigned char*)"");
        r.symbol      = reinterpret_cast<const char*>(sqlite3_column_text(s, 6) ? sqlite3_column_text(s, 6) : (const unsigned char*)"");
        return r;
    }

    static std::string escapeSql(const std::string& s) {
        std::string out;
        for (char c : s) { if (c == '\'') out += '\''; out += c; }
        return out;
    }
};

// ---------------------------------------------------------------------------
// OFX export
// ---------------------------------------------------------------------------

// YYYY-MM-DD → YYYYMMDD
static std::string ofxDate(const std::string& iso) {
    if (iso.size() == 10 && iso[4] == '-')
        return iso.substr(0,4) + iso.substr(5,2) + iso.substr(8,2);
    return iso;
}

// Escape the five XML special characters for OFX element content
static std::string ofxEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

static std::string todayISO() {
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return std::string(buf);
}

static void exportOFX(const std::vector<Record>& records, const std::string& outPath,
                      const std::string& accountName = "") {
    std::string today = todayISO();
    std::vector<const Record*> toExport;
    int skipped = 0;
    for (auto& r : records) {
        if (r.date > today) { ++skipped; continue; }
        toExport.push_back(&r);
    }
    if (skipped > 0)
        std::cout << "Skipping " << skipped << " future-dated record(s) (date > " << today << ").\n";

    std::ofstream f(outPath);
    if (!f) throw std::runtime_error("Cannot write: " + outPath);

    // OFX 1.02 SGML header (no XML declaration)
    f << "OFXHEADER:100\n"
         "DATA:OFXSGML\n"
         "VERSION:102\n"
         "SECURITY:NONE\n"
         "ENCODING:USASCII\n"
         "CHARSET:1252\n"
         "COMPRESSION:NONE\n"
         "OLDFILEUID:NONE\n"
         "NEWFILEUID:NONE\n"
         "\n";

    f << "<OFX>\n"
         "<SIGNONMSGSRSV1><SONRS>"
         "<STATUS><CODE>0</CODE><SEVERITY>INFO</SEVERITY></STATUS>"
         "<DTSERVER>19700101</DTSERVER><LANGUAGE>ENG</LANGUAGE>"
         "</SONRS></SIGNONMSGSRSV1>\n";

    f << "<BANKMSGSRSV1><STMTTRNRS><TRNUID>1001</TRNUID>"
         "<STATUS><CODE>0</CODE><SEVERITY>INFO</SEVERITY></STATUS>\n"
         "<STMTRS><CURDEF>USD</CURDEF>\n";

    std::string acctId = accountName.empty() ? "FYNAB" : accountName;
    f << "<BANKACCTFROM><BANKID>FIDELITY</BANKID>"
      << "<ACCTID>" << ofxEscape(acctId) << "</ACCTID>"
      << "<ACCTTYPE>CHECKING</ACCTTYPE></BANKACCTFROM>\n";

    f << "<BANKTRANLIST>\n";
    for (const Record* rp : toExport) {
        const Record& r = *rp;
        std::string trnType = r.amount >= 0 ? "CREDIT" : "DEBIT";
        char amt[32]; snprintf(amt, sizeof(amt), "%.2f", r.amount);
        // FITID: use db rowid when available, otherwise fall back to a hash of
        // date+amount+description so repeated exports stay stable
        std::string fitid;
        if (r.id > 0) {
            fitid = std::to_string(r.id);
        } else {
            size_t h = std::hash<std::string>{}(r.date + amt + r.description);
            fitid = std::to_string(h);
        }
        f << "<STMTTRN>"
          << "<TRNTYPE>" << trnType << "</TRNTYPE>"
          << "<DTPOSTED>" << ofxDate(r.date) << "</DTPOSTED>"
          << "<TRNAMT>" << amt << "</TRNAMT>"
          << "<FITID>" << fitid << "</FITID>"
          << "<NAME>" << ofxEscape(r.description) << "</NAME>"
          << "</STMTTRN>\n";
    }
    f << "</BANKTRANLIST>\n";

    f << "<LEDGERBAL><BALAMT>0.00</BALAMT><DTASOF>19700101</DTASOF></LEDGERBAL>\n";
    f << "</STMTRS></STMTTRNRS></BANKMSGSRSV1>\n</OFX>\n";

    std::cout << "Exported " << toExport.size() << " records to: " << outPath << "\n";
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

static int dateToDays(const std::string& iso) {
    if (iso.size() != 10) return 0;
    struct tm t = {};
    t.tm_year = std::stoi(iso.substr(0,4)) - 1900;
    t.tm_mon  = std::stoi(iso.substr(5,2)) - 1;
    t.tm_mday = std::stoi(iso.substr(8,2));
    t.tm_isdst = -1;
    time_t tt = mktime(&t);
    return (int)(tt / 86400);
}

static std::vector<Record> runDiff(const std::vector<Record>& dbRecords, const std::string& ynabPath) {
    auto ynabRecords = parseYNABExport(ynabPath);

    std::cout << "\nDB records:   " << dbRecords.size() << "\n";
    std::cout << "YNAB records: " << ynabRecords.size() << "\n\n";

    using Key = std::pair<std::string, std::string>;
    auto amtStr = [](double v) { char b[32]; snprintf(b,sizeof(b),"%.2f",v); return std::string(b); };

    std::map<Key, int> ynabCounts;
    for (auto& r : ynabRecords)
        ynabCounts[{r.date, amtStr(r.amount())}]++;

    std::map<Key, int> dbCounts;
    for (auto& r : dbRecords)
        dbCounts[{r.date, amtStr(r.amount)}]++;

    std::vector<Record> missingFromYNAB;
    std::map<Key,int> ynabRemaining = ynabCounts;
    for (auto& r : dbRecords) {
        Key k = {r.date, amtStr(r.amount)};
        if (ynabRemaining[k] > 0) ynabRemaining[k]--;
        else missingFromYNAB.push_back(r);
    }

    std::vector<YNABRecord> missingFromDB;
    std::map<Key,int> dbRemaining = dbCounts;
    for (auto& r : ynabRecords) {
        Key k = {r.date, amtStr(r.amount())};
        if (dbRemaining[k] > 0) dbRemaining[k]--;
        else missingFromDB.push_back(r);
    }

    // Secondary pass: YNAB "Cash" records that share date+amount with a DB
    // missing record are likely the same transaction — pull them out separately.
    struct ProbablyBoth { Record db; YNABRecord ynab; };
    std::vector<ProbablyBoth> probablyBoth;

    // For each YNAB "Cash" record in missingFromDB, find a DB record in
    // missingFromYNAB with the same amount and a date within 1 day.
    // Prefer exact date match over off-by-one.
    std::set<size_t> dbConsumed;
    std::set<size_t> ynabConsumed;
    for (size_t j = 0; j < missingFromDB.size(); ++j) {
        auto& yr = missingFromDB[j];
        if (toLower(trim(yr.payee)) != "cash") continue;
        int ynabDays = dateToDays(yr.date);
        std::string yAmt = amtStr(yr.amount());

        // Find best match: exact date first, then off-by-one
        size_t bestIdx = SIZE_MAX;
        int bestDiff = 2;
        for (size_t i = 0; i < missingFromYNAB.size(); ++i) {
            if (dbConsumed.count(i)) continue;
            if (amtStr(missingFromYNAB[i].amount) != yAmt) continue;
            int diff = std::abs(dateToDays(missingFromYNAB[i].date) - ynabDays);
            if (diff <= 1 && diff < bestDiff) { bestDiff = diff; bestIdx = i; }
        }
        if (bestIdx == SIZE_MAX) continue;
        dbConsumed.insert(bestIdx);
        ynabConsumed.insert(j);
        probablyBoth.push_back({missingFromYNAB[bestIdx], yr});
    }

    // Rebuild the missing lists without the consumed entries, splitting
    // future-dated DB records into a separate bucket.
    std::string today = todayISO();
    std::vector<Record> confirmedMissingFromYNAB;
    std::vector<Record> futureRecords;
    for (size_t i = 0; i < missingFromYNAB.size(); ++i) {
        if (dbConsumed.count(i)) continue;
        if (missingFromYNAB[i].date > today)
            futureRecords.push_back(missingFromYNAB[i]);
        else
            confirmedMissingFromYNAB.push_back(missingFromYNAB[i]);
    }

    std::vector<YNABRecord> confirmedMissingFromDB;
    for (size_t j = 0; j < missingFromDB.size(); ++j)
        if (!ynabConsumed.count(j)) confirmedMissingFromDB.push_back(missingFromDB[j]);

    if (confirmedMissingFromYNAB.empty() && confirmedMissingFromDB.empty() &&
        probablyBoth.empty() && futureRecords.empty()) {
        std::cout << "✓ Perfect match — all records accounted for on both sides.\n\n";
        return {};
    }

    if (!probablyBoth.empty()) {
        std::cout << "=== Probably In Both — YNAB payee is \"Cash\" (" << probablyBoth.size() << ") ===\n";
        std::cout << std::left << std::setw(22) << "Date (DB/YNAB)" << std::setw(10) << "Amount"
                  << std::setw(40) << "DB Description" << "YNAB Payee\n";
        std::cout << std::string(80, '-') << "\n";
        for (auto& p : probablyBoth) {
            char amt[16]; snprintf(amt, sizeof(amt), "%+.2f", p.db.amount);
            std::string dateCol = p.db.date;
            if (p.db.date != p.ynab.date) dateCol += "/" + p.ynab.date;
            std::cout << std::setw(22) << dateCol << std::setw(10) << amt
                      << std::setw(40) << p.db.description.substr(0, 39)
                      << p.ynab.payee << "\n";
        }
        std::cout << "\n";
    }

    if (!confirmedMissingFromYNAB.empty()) {
        std::cout << "=== In DB but NOT in YNAB (" << confirmedMissingFromYNAB.size() << ") ===\n";
        std::cout << std::left << std::setw(12) << "Date" << std::setw(10) << "Amount" << "Description\n";
        std::cout << std::string(70, '-') << "\n";
        for (auto& r : confirmedMissingFromYNAB) {
            char amt[16]; snprintf(amt, sizeof(amt), "%+.2f", r.amount);
            std::cout << std::setw(12) << r.date << std::setw(10) << amt
                      << r.description.substr(0, 48) << "\n";
        }
        std::cout << "\n";
    }

    if (!confirmedMissingFromDB.empty()) {
        std::cout << "=== In YNAB but NOT in DB (" << confirmedMissingFromDB.size() << ") ===\n";
        std::cout << std::left << std::setw(12) << "Date" << std::setw(10) << "Amount" << "Payee\n";
        std::cout << std::string(70, '-') << "\n";
        for (auto& r : confirmedMissingFromDB) {
            char amt[16]; snprintf(amt, sizeof(amt), "%+.2f", r.amount());
            std::cout << std::setw(12) << r.date << std::setw(10) << amt
                      << r.payee.substr(0, 48) << "\n";
        }
        std::cout << "\n";
    }

    if (!futureRecords.empty()) {
        std::cout << "=== Future-dated — in DB but will NOT be exported (" << futureRecords.size() << ") ===\n";
        std::cout << std::left << std::setw(12) << "Date" << std::setw(10) << "Amount" << "Description\n";
        std::cout << std::string(70, '-') << "\n";
        for (auto& r : futureRecords) {
            char amt[16]; snprintf(amt, sizeof(amt), "%+.2f", r.amount);
            std::cout << std::setw(12) << r.date << std::setw(10) << amt
                      << r.description.substr(0, 48) << "\n";
        }
        std::cout << "\n";
    }

    return confirmedMissingFromYNAB;
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

static void printAccounts(const std::vector<DB::AccountMeta>& accounts) {
    if (accounts.empty()) { std::cout << "(no accounts)\n"; return; }
    std::cout << std::left << std::setw(6) << "ID" << std::setw(30) << "Name" << "Start date\n";
    std::cout << std::string(50, '-') << "\n";
    for (auto& a : accounts)
        std::cout << std::setw(6) << a.id << std::setw(30) << a.name << a.start_date << "\n";
    std::cout << "\n";
}

static void printImports(const std::vector<DB::ImportMeta>& imports) {
    if (imports.empty()) { std::cout << "(no imports)\n"; return; }
    std::cout << std::left
              << std::setw(6)  << "ID"
              << std::setw(20) << "Account"
              << std::setw(22) << "Imported at"
              << std::setw(8)  << "Rows"
              << "Source file\n";
    std::cout << std::string(90, '-') << "\n";
    for (auto& m : imports) {
        std::cout << std::setw(6)  << m.id
                  << std::setw(20) << m.account_name.substr(0, 19)
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
// Interactive menu helpers
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

// Show account list and prompt user to pick one or create a new one.
// Returns the chosen account_id, or 0 on cancel.
static int64_t pickOrCreateAccount(DB& db) {
    auto accounts = db.listAccounts();
    if (!accounts.empty()) {
        printAccounts(accounts);
        std::cout << "Enter account ID, or 0 to create a new account: ";
        std::string line;
        std::getline(std::cin, line);
        int64_t id = -1;
        try { id = std::stoll(trim(line)); } catch (...) {}
        if (id == 0) {
            // fall through to create
        } else {
            if (db.accountExists(id)) return id;
            std::cout << "Account not found.\n";
            return 0;
        }
    }
    std::string name = promptString("New account name: ");
    if (name.empty()) { std::cout << "Cancelled.\n"; return 0; }
    std::string start_date = promptString("Start date (YYYY-MM-DD): ");
    start_date = normalizeDate(start_date);
    if (start_date.size() != 10) { std::cout << "Invalid date.\n"; return 0; }
    int64_t id = db.createAccount(name, start_date);
    std::cout << "Created account \"" << name << "\" starting " << start_date << " (ID " << id << ").\n";
    return id;
}

// ---------------------------------------------------------------------------
// Menu actions
// ---------------------------------------------------------------------------

static void menuManageAccounts(DB& db) {
    while (true) {
        std::cout << "\n-- Accounts --\n";
        printAccounts(db.listAccounts());
        std::cout << " a) Create account\n s) Set start date\n d) Delete account\n b) Back\n> ";
        std::string ch;
        std::getline(std::cin, ch);
        ch = trim(ch);
        if (ch == "a") {
            std::string name = promptString("Account name: ");
            if (name.empty()) continue;
            std::string start_date = promptString("Start date (YYYY-MM-DD): ");
            start_date = normalizeDate(start_date);
            if (start_date.size() != 10) { std::cout << "Invalid date.\n"; continue; }
            int64_t id = db.createAccount(name, start_date);
            std::cout << "Created account \"" << name << "\" starting " << start_date << " (ID " << id << ").\n";
        } else if (ch == "s") {
            int64_t id = promptInt("Account ID: ");
            if (!db.accountExists(id)) { std::cout << "Not found.\n"; continue; }
            std::string start_date = promptString("New start date (YYYY-MM-DD): ");
            start_date = normalizeDate(start_date);
            if (start_date.size() != 10) { std::cout << "Invalid date.\n"; continue; }
            db.updateAccountStartDate(id, start_date);
            std::cout << "Updated.\n";
        } else if (ch == "d") {
            int64_t id = promptInt("Account ID to delete: ");
            if (!db.accountExists(id)) { std::cout << "Not found.\n"; continue; }
            if (!confirm("Delete account " + std::to_string(id) +
                         "? (imports will be orphaned)")) continue;
            db.deleteAccount(id);
            std::cout << "Deleted.\n";
        } else if (ch == "b" || ch.empty()) {
            break;
        } else {
            std::cout << "Unknown option.\n";
        }
    }
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

    int64_t account_id = pickOrCreateAccount(db);
    if (account_id <= 0) return;

    std::string start_date = db.accountStartDate(account_id);
    int beforeStart = 0;
    if (!start_date.empty()) {
        std::vector<Record> filtered;
        for (auto& r : parsed) {
            if (r.date < start_date) ++beforeStart;
            else filtered.push_back(r);
        }
        parsed = std::move(filtered);
        if (beforeStart > 0)
            std::cout << "Ignoring " << beforeStart << " record(s) before account start date " << start_date << ".\n";
    }

    auto dbKeys = db.existingKeys(account_id);

    std::vector<Record> newRecords;
    std::vector<Record> dbDups;

    for (auto& r : parsed) {
        auto key = std::make_tuple(r.date, r.amount, r.description);
        if (dbKeys.count(key))
            dbDups.push_back(r);
        else
            newRecords.push_back(r);
    }

    if (!dbDups.empty()) {
        std::cout << "\nAlready in database (" << dbDups.size() << ") — will skip:\n";
        std::cout << std::left << std::setw(12) << "Date"
                  << std::setw(10) << "Amount" << "Description\n";
        std::cout << std::string(70, '-') << "\n";
        for (auto& r : dbDups) {
            char amt[16]; snprintf(amt, sizeof(amt), "%+.2f", r.amount);
            std::cout << std::setw(12) << r.date << std::setw(10) << amt
                      << r.description.substr(0, 48) << "\n";
        }
        std::cout << "\n";
    }

    std::cout << newRecords.size() << " new record(s) to import.\n";

    if (newRecords.empty()) { std::cout << "Nothing to import.\n"; return; }
    if (!confirm("Proceed?")) return;

    db.exec("BEGIN;");
    int64_t imp_id = db.insertImport(account_id, fs::absolute(path).string(), (int)newRecords.size());
    for (auto& r : newRecords) {
        r.import_id = imp_id;
        db.insertRecord(r);
    }
    db.exec("COMMIT;");
    std::cout << "Imported " << newRecords.size() << " records (import ID " << imp_id << ").\n";
}

static void menuListImports(DB& db) {
    auto accounts = db.listAccounts();
    int64_t filter = 0;
    if (!accounts.empty()) {
        printAccounts(accounts);
        filter = promptInt("Filter by account ID (0 = all): ");
        if (filter < 0) filter = 0;
    }
    printImports(db.listImports(filter));
}

static void menuListRecords(DB& db) {
    std::cout << " 1) By import ID\n 2) By account\n 3) All\n> ";
    std::string ch; std::getline(std::cin, ch); ch = trim(ch);
    if (ch == "1") {
        int64_t id = promptInt("Import ID: ");
        if (!db.importExists(id)) { std::cout << "Import not found.\n"; return; }
        printRecords(db.recordsForImport(id));
    } else if (ch == "2") {
        printAccounts(db.listAccounts());
        int64_t id = promptInt("Account ID: ");
        if (!db.accountExists(id)) { std::cout << "Account not found.\n"; return; }
        printRecords(db.recordsForAccount(id));
    } else {
        printRecords(db.allRecords());
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
    std::cout << " 1) By import ID\n 2) By account\n 3) All records\n> ";
    std::string ch; std::getline(std::cin, ch); ch = trim(ch);

    std::vector<Record> records;
    std::string accountName;
    if (ch == "1") {
        int64_t id = promptInt("Import ID: ");
        if (!db.importExists(id)) { std::cout << "Import not found.\n"; return; }
        records = db.recordsForImport(id);
    } else if (ch == "2") {
        auto accounts = db.listAccounts();
        printAccounts(accounts);
        int64_t id = promptInt("Account ID: ");
        if (!db.accountExists(id)) { std::cout << "Account not found.\n"; return; }
        records = db.recordsForAccount(id);
        for (auto& a : accounts) if (a.id == id) { accountName = a.name; break; }
    } else {
        records = db.allRecords();
    }

    if (records.empty()) { std::cout << "No records to export.\n"; return; }
    std::string outPath = promptString("Output OFX path [ynab_import.ofx]: ");
    if (outPath.empty()) outPath = "ynab_import.ofx";
    exportOFX(records, outPath, accountName);
}

static void menuDiff(DB& db) {
    std::vector<Record> dbRecords;
    auto accounts = db.listAccounts();
    if (!accounts.empty()) {
        printAccounts(accounts);
        int64_t id = promptInt("Account ID to diff (0 = all): ");
        if (id > 0) {
            if (!db.accountExists(id)) { std::cout << "Account not found.\n"; return; }
            dbRecords = db.recordsForAccount(id);
        } else {
            dbRecords = db.allRecords();
        }
    } else {
        dbRecords = db.allRecords();
    }

    std::string path = promptString("Path to YNAB export CSV: ");
    if (path.empty() || !fs::exists(path)) {
        std::cout << "File not found.\n"; return;
    }
    auto missingFromYNAB = runDiff(dbRecords, path);

    if (!missingFromYNAB.empty() &&
        confirm("Export the " + std::to_string(missingFromYNAB.size()) +
                " DB-only record(s) as a YNAB import OFX?")) {
        std::string outPath = promptString("Output OFX path [ynab_import.ofx]: ");
        if (outPath.empty()) outPath = "ynab_import.ofx";
        exportOFX(missingFromYNAB, outPath);
    }
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
            } catch (std::exception& e) {
                std::cerr << "Could not open database (wrong password?): " << e.what() << "\n";
                return 1;
            }
        }
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    while (true) {
        std::cout << "------------------------------------\n";
        std::cout << " 1) Import Fidelity CSV\n";
        std::cout << " 2) List imports\n";
        std::cout << " 3) List records\n";
        std::cout << " 4) Export to YNAB OFX\n";
        std::cout << " 5) Diff against YNAB export\n";
        std::cout << " 6) Delete an import\n";
        std::cout << " 7) Delete a single record\n";
        std::cout << " 8) Manage accounts\n";
        std::cout << " 0) Quit\n";
        std::cout << "------------------------------------\n";
        std::cout << "> ";

        std::string choice;
        if (!std::getline(std::cin, choice)) break;
        choice = trim(choice);

        try {
            if      (choice == "1") menuImport(db);
            else if (choice == "2") menuListImports(db);
            else if (choice == "3") menuListRecords(db);
            else if (choice == "4") menuExport(db);
            else if (choice == "5") menuDiff(db);
            else if (choice == "6") menuDeleteImport(db);
            else if (choice == "7") menuDeleteRecord(db);
            else if (choice == "8") menuManageAccounts(db);
            else if (choice == "0") break;
            else std::cout << "Unknown option.\n";
        } catch (std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
    }

    return 0;
}
