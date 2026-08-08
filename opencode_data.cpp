// OpenCode Session Manager -- shared data layer implementation
#include "opencode_data.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

// ---------------------------------------------------------------------------
// small path helpers
// ---------------------------------------------------------------------------
string path_join(const string& base, const string& name) {
    if (base.empty()) return name;
    char c = base.back();
    if (c == '/' || c == '\\') return base + name;
    return base + "/" + name;
}

bool path_exists(const string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
#else
    (void)p;
    return false;
#endif
}

bool is_dir(const string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p;
    return false;
#endif
}

std::vector<string> list_children(const string& p) {
    std::vector<string> out;
#ifdef _WIN32
    string pat = p;
    if (!pat.empty() && pat.back() != '/' && pat.back() != '\\') pat += "\\";
    pat += "*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)p;
#endif
    return out;
}

std::vector<string> list_child_dirs(const string& p) {
    std::vector<string> out;
    for (auto& n : list_children(p)) {
        string full = path_join(p, n);
        if (is_dir(full)) out.push_back(full);
    }
    return out;
}

std::vector<string> list_child_files(const string& p) {
    std::vector<string> out;
    for (auto& n : list_children(p)) {
        string full = path_join(p, n);
        if (!is_dir(full)) out.push_back(full);
    }
    return out;
}

string file_extension(const string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == string::npos) return "";
    if (slash != string::npos && dot < slash) return "";
    return path.substr(dot);
}

string file_stem(const string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t start = (slash == string::npos) ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == string::npos || dot < start) return path.substr(start);
    return path.substr(start, dot - start);
}

long long total_dir_size(const string& p) {
    long long total = 0;
#ifdef _WIN32
    for (auto& n : list_children(p)) {
        string child = path_join(p, n);
        if (is_dir(child)) {
            total += total_dir_size(child);
        } else {
            struct _stat st;
            if (_stat(child.c_str(), &st) == 0) total += st.st_size;
        }
    }
#else
    (void)p;
#endif
    return total;
}

void remove_tree(const string& p) {
#ifdef _WIN32
    for (auto& n : list_children(p)) {
        string child = path_join(p, n);
        if (is_dir(child)) remove_tree(child);
        else DeleteFileA(child.c_str());
    }
    RemoveDirectoryA(p.c_str());
#else
    (void)p;
#endif
}

void delete_file(const string& p) {
#ifdef _WIN32
    DeleteFileA(p.c_str());
#else
    (void)p;
#endif
}

// ---------------------------------------------------------------------------
// Config / paths
// ---------------------------------------------------------------------------
string home_dir() {
#ifdef _WIN32
    {
        const char* h = getenv("USERPROFILE");
        if (h && *h) return h;
    }
#endif
    {
        const char* h = getenv("HOME");
        if (h && *h) return h;
    }
    return ".";
}

string data_dir() {
    const char* env = getenv("OPENCODE_DATA_DIR");
    if (env && *env) return env;
    return path_join(path_join(path_join(home_dir(), ".local"), "share"), "opencode");
}

string db_path()       { return path_join(data_dir(), "opencode.db"); }
string diff_dir()      { return path_join(path_join(data_dir(), "storage"), "session_diff"); }
string snapshot_dir()  { return path_join(data_dir(), "snapshot"); }

// ---------------------------------------------------------------------------
// UTF-8 <-> wide helpers (Windows)
// ---------------------------------------------------------------------------
wstring utf8_to_wide(const string& s) {
    if (s.empty()) return L"";
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
#else
    wstring out;
    for (char c : s) out += (wchar_t)(unsigned char)c;
#endif
    return out;
}

string wide_to_utf8(const wstring& w) {
    if (w.empty()) return "";
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
#else
    string out;
    for (wchar_t c : w) out += (char)c;
    return out;
#endif
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
wstring format_time(long long ts_ms) {
    if (ts_ms <= 0) return L"-";
    time_t t = (time_t)(ts_ms / 1000);
    struct tm tmv, tmn;
#ifdef _WIN32
    localtime_s(&tmv, &t);
    time_t now = time(nullptr);
    localtime_s(&tmn, &now);
#else
    localtime_r(&t, &tmv);
    time_t now = time(nullptr);
    localtime_r(&now, &tmn);
#endif
    wchar_t buf[64];
    bool same_yday = (tmv.tm_yday == tmn.tm_yday) && (tmv.tm_year == tmn.tm_year);
    if (same_yday) {
        swprintf(buf, 64, L"今天 %02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else if (tmv.tm_year == tmn.tm_year) {
        swprintf(buf, 64, L"%02d-%02d %02d:%02d",
                 tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    } else {
        swprintf(buf, 64, L"%04d-%02d-%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }
    return buf;
}

wstring format_size(long long size_bytes) {
    wchar_t buf[32];
    if (size_bytes < 1024)
        swprintf(buf, 32, L"%lldB", size_bytes);
    else if (size_bytes < 1024LL * 1024)
        swprintf(buf, 32, L"%.1fK", size_bytes / 1024.0);
    else if (size_bytes < 1024LL * 1024 * 1024)
        swprintf(buf, 32, L"%.1fM", size_bytes / (1024.0 * 1024));
    else
        swprintf(buf, 32, L"%.1fG", size_bytes / (1024.0 * 1024 * 1024));
    return buf;
}

wstring truncate(const wstring& text, size_t width) {
    if (text.size() <= width) return text;
    if (width == 0) return L"";
    return text.substr(0, width - 1) + L"…";
}

wstring ljust(const wstring& text, size_t width) {
    wstring t = truncate(text, width);
    if (t.size() < width) t.append(width - t.size(), L' ');
    return t;
}

wstring rjust(const wstring& text, size_t width) {
    wstring t = truncate(text, width);
    if (t.size() < width) return wstring(width - t.size(), L' ') + t;
    return t;
}

// ---------------------------------------------------------------------------
// DB access (sqlite3)
// ---------------------------------------------------------------------------
sqlite3* open_db(const string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("Failed to open database: " + path);
    }
    return db;
}

string col_text(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

std::vector<Session> load_sessions(const string& path) {
    std::vector<Session> out;
    sqlite3* db = open_db(path);
    const char* sql =
        "SELECT s.id, s.title, s.time_created, s.time_updated,"
        "       p.name AS project_name, p.worktree,"
        "       (SELECT COUNT(*) FROM message WHERE session_id = s.id) AS msg_count"
        " FROM session s"
        " JOIN project p ON s.project_id = p.id"
        " ORDER BY s.time_created DESC";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        Session s;
        s.id = col_text(st, 0);
        s.title = utf8_to_wide(col_text(st, 1));
        s.time_created = sqlite3_column_int64(st, 2);
        s.time_updated = sqlite3_column_int64(st, 3);
        s.project_name = col_text(st, 4);
        s.worktree = col_text(st, 5);
        s.msg_count = sqlite3_column_int64(st, 6);
        out.push_back(std::move(s));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

std::vector<Snapshot> load_snapshots(const string& path, const string& snap_dir) {
    std::vector<Snapshot> out;
    std::map<string, std::pair<string, string>> project_map;
    sqlite3* db = open_db(path);
    sqlite3_stmt* st = nullptr;

    if (sqlite3_prepare_v2(db, "SELECT id, worktree, name FROM project", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            project_map[col_text(st, 0)] = {col_text(st, 1), col_text(st, 2)};
        }
    }
    sqlite3_finalize(st);

    std::set<string> active_ids;
    if (sqlite3_prepare_v2(db, "SELECT DISTINCT project_id FROM session", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) active_ids.insert(col_text(st, 0));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (!is_dir(snap_dir)) return out;

    std::vector<string> dirs = list_child_dirs(snap_dir);
    std::sort(dirs.begin(), dirs.end());

    for (auto& d : dirs) {
        Snapshot sn;
        sn.path = d;
        sn.id = file_stem(d);
        sn.size = total_dir_size(d);
        auto it = project_map.find(sn.id);
        if (it != project_map.end()) {
            sn.worktree = it->second.first;
            sn.name = utf8_to_wide(it->second.second);
        } else {
            sn.worktree = "(deleted project)";
        }
        sn.active_sessions = active_ids.count(sn.id) > 0;
        out.push_back(std::move(sn));
    }
    return out;
}

int delete_sessions(const string& path, const std::vector<string>& ids) {
    sqlite3* db = open_db(path);
    sqlite3_exec(db, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM session WHERE id = ?", -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    int deleted = 0;
    for (auto& id : ids) {
        sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        deleted += sqlite3_changes(db);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return deleted;
}

std::vector<string> cleanup_orphan_diffs(const string& path, const string& diff_dir) {
    std::vector<string> removed;
    std::set<string> existing;
    sqlite3* db = open_db(path);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id FROM session", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) existing.insert(col_text(st, 0));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (!is_dir(diff_dir)) return removed;
    for (auto& f : list_child_files(diff_dir)) {
        if (file_extension(f) == ".json") {
            string sid = file_stem(f);
            if (!existing.count(sid)) {
                delete_file(f);
                removed.push_back(sid);
            }
        }
    }
    return removed;
}