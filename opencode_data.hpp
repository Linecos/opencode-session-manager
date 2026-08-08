// OpenCode Session Manager -- shared data layer (used by TUI and GUI)
#pragma once

#include <sqlite3.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <sys/stat.h>
#include <windows.h>
#endif

using std::string;
using std::wstring;

// ---------------------------------------------------------------------------
// small path helpers (std::filesystem is broken on GCC 8/MinGW)
// ---------------------------------------------------------------------------
string path_join(const string& base, const string& name);
bool path_exists(const string& p);
bool is_dir(const string& p);
std::vector<string> list_children(const string& p);
std::vector<string> list_child_dirs(const string& p);
std::vector<string> list_child_files(const string& p);
string file_extension(const string& path);
string file_stem(const string& path);
long long total_dir_size(const string& p);
void remove_tree(const string& p);
void delete_file(const string& p);

// ---------------------------------------------------------------------------
// Config / paths
// ---------------------------------------------------------------------------
string home_dir();
string data_dir();
string db_path();
string diff_dir();
string snapshot_dir();

// ---------------------------------------------------------------------------
// UTF-8 <-> wide helpers (Windows)
// ---------------------------------------------------------------------------
wstring utf8_to_wide(const string& s);
string wide_to_utf8(const wstring& w);

// ---------------------------------------------------------------------------
// Formatting helpers (mirror format_time / format_size / truncate)
// ---------------------------------------------------------------------------
wstring format_time(long long ts_ms);
wstring format_size(long long size_bytes);
wstring truncate(const wstring& text, size_t width);
wstring ljust(const wstring& text, size_t width);
wstring rjust(const wstring& text, size_t width);

// ---------------------------------------------------------------------------
// Data models
// ---------------------------------------------------------------------------
struct Session {
    string id;
    wstring title;
    long long time_created = 0;
    long long time_updated = 0;
    string project_name;
    string worktree;
    long long msg_count = 0;
};

struct Snapshot {
    string id;            // project_id / directory name
    string path;          // directory on disk
    long long size = 0;
    string worktree;
    wstring name;
    bool active_sessions = false;
};

// ---------------------------------------------------------------------------
// DB access (sqlite3)
// ---------------------------------------------------------------------------
sqlite3* open_db(const string& path);
string col_text(sqlite3_stmt* st, int i);
std::vector<Session> load_sessions(const string& path);
std::vector<Snapshot> load_snapshots(const string& path, const string& snap_dir);
int delete_sessions(const string& path, const std::vector<string>& ids);
std::vector<string> cleanup_orphan_diffs(const string& path, const string& diff_dir);