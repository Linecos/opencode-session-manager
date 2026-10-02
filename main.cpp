// OpenCode Session Manager -- terminal UI (ncursesw).
//
// Browse and clean old OpenCode conversations & orphan snapshot directories.
// Feature set mirrors the GUI: filter, sort, page navigation, reveal in
// Explorer, on-demand orphan diff cleanup and progressive snapshot sizing.
//
// Build (MinGW-w64, ncursesw + sqlite3 from C:\MinGW\opt), one line:
//   g++ -std=c++17 -O2 -DNCURSES_WIDECHAR -I"C:\MinGW\opt\include" main.cpp opencode_data.cpp
//        -L"C:\MinGW\opt\lib" -lncursesw -lsqlite3 -luser32 -lshell32 -lole32
//        -o opencode-session-manager.exe
//
// Optional env override for data location (handy for testing):
//   OPENCODE_DATA_DIR=<dir containing opencode.db, storage/, snapshot/>
//
// Key bindings (also shown in the on-screen help):
//   up/down, j/k      move cursor          PgUp/PgDn, Home/End   page / jump
//   Space             toggle selection     Ctrl+A / Ctrl+D       all / none
//   Tab               switch view          F5                    reload data
//   /                 filter               Esc                   clear filter, else quit
//   s / S             cycle sort field / reverse direction
//   d / Enter         delete selected (confirm with y/n)
//   c                 copy resume command (opencode -s <id>)
//   o                 resume session in a new console
//   r                 reveal in Explorer
//   O                 snapshots: orphan directories only
//   x                 clean orphan session_diff files now
//   Ctrl+E            toggle cleanup-on-exit
//   q                 quit
//
// Removals go to the Windows Recycle Bin by default; set OPENCODE_SM_NO_RECYCLE=1
// to delete permanently instead.

#define NCURSES_WIDECHAR 1
#include <ncursesw/ncurses.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>   // SetConsoleOutputCP
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "opencode_data.hpp"
#include "session_view.hpp"
#include "version.h"

using std::string;
using std::wstring;

// ---------------------------------------------------------------------------
// Color pair ids (keep names from original)
// ---------------------------------------------------------------------------
enum {
    COLOR_NORMAL = 1,
    COLOR_SELECTED,
    COLOR_HEADER,
    COLOR_HELP,
    COLOR_WARN,
    COLOR_DIM,
    COLOR_TAB_ACTIVE,
    COLOR_TAB_INACTIVE,
    COLOR_FILTER
};

// ---------------------------------------------------------------------------
// ncurses helpers
// ---------------------------------------------------------------------------
static void waddswstr(WINDOW* win, const wstring& s) {
    waddnwstr(win, s.c_str(), (int)s.size());
}

static void mvwaddswstr(WINDOW* win, int y, int x, const wstring& s) {
    wmove(win, y, x);
    waddswstr(win, s);
}

// UTF-8 text helpers for the filter box (typed text arrives as raw bytes, so a
// pasted CJK query still forms a valid UTF-8 string).
static bool utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static void utf8_pop_back(string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && utf8_cont((unsigned char)s[i])) --i;
    s.erase(i);
}

// ---------------------------------------------------------------------------
// Sort modes
// ---------------------------------------------------------------------------
enum {
    TAB_SESSIONS = 0,
    TAB_SNAPSHOTS = 1
};

// Which fields the 's' key cycles through, per view.
static const SortField kSessionSortFields[] = {SORT_CREATED, SORT_UPDATED, SORT_MESSAGES,
                                               SORT_NAME, SORT_LOCATION};
static const SortField kSnapshotSortFields[] = {SORT_SIZE, SORT_ID, SORT_NAME, SORT_STATUS};
static const int kSessionSortCount = (int)(sizeof(kSessionSortFields) / sizeof(SortField));
static const int kSnapshotSortCount = (int)(sizeof(kSnapshotSortFields) / sizeof(SortField));

// ---------------------------------------------------------------------------
// TUI application
// ---------------------------------------------------------------------------
static const string DB_PATH = db_path();
static const string DIFF_DIR = diff_dir();
static const string SNAPSHOT_DIR = snapshot_dir();

// Snapshot sizes are computed a few per redraw so the UI never stalls on a large
// data directory (the GUI does the same, a frame at a time).
static const int SIZE_SCAN_PER_PUMP = 8;

class App {
public:
    int tab = TAB_SESSIONS;
    std::vector<Session> sessions;
    std::vector<Snapshot> snapshots;
    std::vector<int> view;          // row -> index into sessions/snapshots
    std::set<string> selected;      // session id, or snapshot path
    int cursor = 0;
    int scroll = 0;
    int page_rows = 10;
    bool confirm_mode = false;
    bool filter_mode = false;
    string filter;                  // applied query
    string filter_edit;             // query being typed
    bool orphans_only = false;
    SortField session_sort = SORT_CREATED;
    SortField snapshot_sort = SORT_SIZE;
    bool session_desc = true;
    bool snapshot_desc = true;
    bool cleanup_on_exit = false;
    size_t scan_next = 0;
    size_t scan_pending = 0;
    string message;
    bool running = true;

    void run() {
        try {
            init_tui();
            refresh_data(false);
            while (running) {
                draw();
                // Poll while sizes are still being measured so the scan can make
                // progress without the user having to press a key.
                if (scan_pending) {
                    timeout(50);
                    int key = getch();
                    if (key == ERR) {
                        pump_sizes(SIZE_SCAN_PER_PUMP);
                        continue;
                    }
                    handle_key(key);
                } else {
                    timeout(-1);
                    handle_key(getch());
                }
            }
        } catch (const std::exception& e) {
            cleanup_screen();
            fprintf(stderr, "Error: %s\n", e.what());
            throw;
        }
        cleanup_screen();
    }

private:
    void init_tui() {
        curs_set(0);
        start_color();
        use_default_colors();
        init_pair(COLOR_NORMAL, -1, -1);
        init_pair(COLOR_SELECTED, COLOR_GREEN, -1);
        init_pair(COLOR_HEADER, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_HELP, COLOR_BLACK, COLOR_WHITE);
        init_pair(COLOR_WARN, COLOR_RED, -1);
        init_pair(COLOR_DIM, 8, -1);
        init_pair(COLOR_TAB_ACTIVE, COLOR_BLACK, COLOR_GREEN);
        init_pair(COLOR_TAB_INACTIVE, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_FILTER, COLOR_YELLOW, -1);
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
    }

    void cleanup_screen() { endwin(); }

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    void refresh_data(bool keep_view) {
        sessions = load_sessions(DB_PATH);
        // List only: sizes are measured progressively by pump_sizes().
        snapshots = load_snapshots(DB_PATH, SNAPSHOT_DIR, /*with_sizes=*/false);
        scan_next = 0;
        scan_pending = snapshots.size();

        if (!keep_view) {
            selected.clear();
            cursor = 0;
            scroll = 0;
        } else {
            prune_selection();
        }
        rebuild_view();
    }

    void pump_sizes(int budget) {
        const size_t n = snapshots.size();
        if (!n || scan_pending == 0) return;

        int done = 0, scanned = 0;
        while (done < budget && scanned < (int)n) {
            size_t i = scan_next % n;
            scan_next = (i + 1) % n;
            ++scanned;
            if (snapshots[i].size_known) continue;
            snapshots[i].size = total_dir_size(snapshots[i].path);
            snapshots[i].size_known = true;
            --scan_pending;
            ++done;
        }
        // Re-sort only once the scan is complete, otherwise rows would move
        // around under the cursor while sizes trickle in.
        if (scan_pending == 0) {
            if (tab == TAB_SNAPSHOTS && snapshot_sort == SORT_SIZE) rebuild_view();
        }
    }

    // -----------------------------------------------------------------------
    // Filtering / sorting / view
    // -----------------------------------------------------------------------
    // The query in effect: while typing it is the edit buffer, so Esc can drop
    // the whole edit without ever having touched the applied filter.
    const string& active_filter() const { return filter_mode ? filter_edit : filter; }

    void rebuild_view() {
        ViewQuery q;
        q.filter = active_filter();
        q.orphans_only = orphans_only;
        if (tab == TAB_SESSIONS) {
            q.field = session_sort;
            q.descending = session_desc;
            view = build_session_view(sessions, q);
        } else {
            q.field = snapshot_sort;
            q.descending = snapshot_desc;
            view = build_snapshot_view(snapshots, q);
        }
        clamp_state();
    }

    size_t total_rows() const { return tab == TAB_SESSIONS ? sessions.size() : snapshots.size(); }
    size_t visible_rows() const { return view.size(); }

    string key_at(int row) const {
        if (row < 0 || row >= (int)view.size()) return string();
        int idx = view[(size_t)row];
        return tab == TAB_SESSIONS ? sessions[(size_t)idx].id : snapshots[(size_t)idx].path;
    }

    void clamp_state() {
        int n = (int)view.size();
        if (n <= 0) {
            cursor = 0;
            scroll = 0;
            return;
        }
        if (cursor > n - 1) cursor = n - 1;
        if (cursor < 0) cursor = 0;
        if (scroll < 0) scroll = 0;
        if (scroll > cursor) scroll = cursor;
    }

    void prune_selection() {
        std::set<string> alive;
        for (auto& s : sessions) alive.insert(s.id);
        for (auto& s : snapshots) alive.insert(s.path);
        for (auto it = selected.begin(); it != selected.end();) {
            if (!alive.count(*it)) it = selected.erase(it);
            else ++it;
        }
    }

    // -----------------------------------------------------------------------
    // Input
    // -----------------------------------------------------------------------
    void handle_key(int key) {
        if (key == ERR) return;

        if (filter_mode) {
            handle_filter_key(key);
            return;
        }
        if (confirm_mode) {
            if (key == 'y' || key == 'Y') do_delete();
            confirm_mode = false;
            return;
        }

        switch (key) {
        case 'q':
        case 'Q':
            running = false;
            break;
        case 27:                                  // Esc: clear filter first, then quit
            if (!filter.empty()) {
                filter.clear();
                rebuild_view();
                message = "Filter cleared.";
            } else {
                running = false;
            }
            break;        case '\t':
            tab = 1 - tab;
            selected.clear();
            cursor = 0;
            scroll = 0;
            message.clear();
            rebuild_view();
            break;
        case KEY_UP:
        case 'k':
            if (cursor > 0) cursor--;
            break;
        case KEY_DOWN:
        case 'j':
            if (cursor < (int)visible_rows() - 1) cursor++;
            break;
        case KEY_PPAGE:
            cursor -= page_rows;
            if (cursor < 0) cursor = 0;
            break;
        case KEY_NPAGE:
            cursor += page_rows;
            if (cursor > (int)visible_rows() - 1) cursor = (int)visible_rows() - 1;
            if (cursor < 0) cursor = 0;
            break;
        case KEY_HOME:
            cursor = 0;
            break;
        case KEY_END:
            cursor = (int)visible_rows() - 1;
            if (cursor < 0) cursor = 0;
            break;
        case ' ': {
            string k = key_at(cursor);
            if (!k.empty()) {
                if (selected.count(k)) selected.erase(k);
                else selected.insert(k);
            }
            break;
        }
        case 1:                                   // Ctrl+A
            selected.clear();
            for (size_t i = 0; i < visible_rows(); ++i) selected.insert(key_at((int)i));
            message = "Selected " + std::to_string(selected.size()) + " visible item(s).";
            break;
        case 4:                                   // Ctrl+D
            selected.clear();
            break;
        case '/':
            filter_mode = true;
            filter_edit = filter;
            break;
        case 's':
            if (tab == TAB_SESSIONS) {
                int i = 0;
                while (i < kSessionSortCount && kSessionSortFields[i] != session_sort) ++i;
                session_sort = kSessionSortFields[(i + 1) % kSessionSortCount];
            } else {
                int i = 0;
                while (i < kSnapshotSortCount && kSnapshotSortFields[i] != snapshot_sort) ++i;
                snapshot_sort = kSnapshotSortFields[(i + 1) % kSnapshotSortCount];
            }
            rebuild_view();
            message = "Sort: " + string(sort_field_name(current_sort_field())) +
                      (current_sort_desc() ? " (desc)" : " (asc)");
            break;
        case 'S':
            if (tab == TAB_SESSIONS) session_desc = !session_desc;
            else snapshot_desc = !snapshot_desc;
            rebuild_view();
            message = "Sort: " + string(sort_field_name(current_sort_field())) +
                      (current_sort_desc() ? " (desc)" : " (asc)");
            break;
        case 'O':
            orphans_only = !orphans_only;
            if (tab != TAB_SNAPSHOTS) tab = TAB_SNAPSHOTS;
            selected.clear();
            cursor = 0;
            scroll = 0;
            rebuild_view();
            message = orphans_only ? "Showing orphan snapshot directories only."
                                   : "Showing all snapshot directories.";
            break;
        case 'c':
        case 'C':
            copy_command();
            break;
        case 'o':
            jump_session();
            break;
        case 'r':
            reveal_path();
            break;
        case 'x':
            clean_orphan_diffs_now();
            break;
        case 5:                                   // Ctrl+E
            cleanup_on_exit = !cleanup_on_exit;
            message = string("Cleanup orphan diffs on exit: ") + (cleanup_on_exit ? "ON" : "OFF");
            break;
        case 'd':
        case 'D':
        case 10:
        case 13:
        case KEY_ENTER:
            if (!selected.empty()) confirm_mode = true;
            else message = "Nothing selected.";
            break;
        case KEY_F(5):
            refresh_data(true);
            message = "Reloaded.";
            break;
        default:
            break;
        }
    }

    void handle_filter_key(int key) {
        switch (key) {
        case 10:
        case 13:
        case KEY_ENTER:
            filter = filter_edit;
            filter_mode = false;
            rebuild_view();
            cursor = 0;
            scroll = 0;
            message = filter.empty() ? "Filter cleared."
                                     : "Filter: \"" + filter + "\" -> " +
                                           std::to_string(visible_rows()) + " match(es)";
            break;
        case 27:                                  // Esc cancels the edit
            filter_edit = filter;
            filter_mode = false;
            rebuild_view();
            clamp_state();
            break;
        case KEY_BACKSPACE:
        case 127:
        case 8:
            utf8_pop_back(filter_edit);
            break;
        default:
            // Printable bytes (including multi-byte UTF-8, so a pasted CJK query
            // works). Filter live while typing.
            if (key >= 32 && key < 256) {
                filter_edit += (char)key;
                rebuild_view();
                clamp_state();
            }
            break;
        }
    }

    void copy_command() {
        if (tab != TAB_SESSIONS) {
            message = "Resume command copy is only available for sessions.";
            return;
        }
        if (view.empty()) return;
        const Session& s = sessions[(size_t)view[(size_t)cursor]];
        string cmd = "opencode -s " + s.id;
        string err;
        if (set_clipboard_text(cmd, &err)) message = "Copied: " + cmd;
        else message = "Error: " + err;
    }

    void jump_session() {
        if (tab != TAB_SESSIONS) {
            message = "Jump is only available for sessions.";
            return;
        }
        if (view.empty()) return;
        const Session& s = sessions[(size_t)view[(size_t)cursor]];
        string err;
        if (launch_opencode_session(s.id, s.worktree, &err)) message = "Resumed session " + s.id;
        else message = "Error: " + err;
    }

    void reveal_path() {
        if (view.empty()) return;
        string path, err;
        if (tab == TAB_SESSIONS) {
            path = sessions[(size_t)view[(size_t)cursor]].worktree;
            if (path.empty()) {
                message = "This session's project directory is unknown.";
                return;
            }
        } else {
            path = snapshots[(size_t)view[(size_t)cursor]].path;
        }
        if (reveal_in_explorer(path, &err)) message = "Opened in Explorer: " + path;
        else message = "Error: " + err;
    }

    void clean_orphan_diffs_now() {
        try {
            std::vector<string> failed;
            auto orphans = cleanup_orphan_diffs(DB_PATH, DIFF_DIR, &failed);
            if (orphans.empty() && failed.empty())
                message = "No orphan session_diff files to clean.";
            else
                message = (delete_to_recycle() ? "Moved " : "Removed ") +
                          std::to_string(orphans.size()) + " orphan diff file(s)" +
                          (failed.empty() ? "" : ", " + std::to_string(failed.size()) + " failed");
        } catch (const std::exception& e) {
            message = string("Error: ") + e.what();
        }
    }

    void do_delete() {
        if (tab == TAB_SESSIONS) delete_sessions_ui();
        else delete_snapshots_ui();
    }

    void delete_sessions_ui() {
        std::vector<string> ids(selected.begin(), selected.end());
        if (ids.empty()) return;
        try {
            int n = delete_sessions(DB_PATH, ids);
            std::vector<string> failed;
            auto orphans = cleanup_orphan_diffs(DB_PATH, DIFF_DIR, &failed);
            message = "Deleted " + std::to_string(n) + " session(s)";
            if (!orphans.empty())
                message += (delete_to_recycle() ? ", moved " : ", removed ") +
                           std::to_string(orphans.size()) + " orphan diff(s)";
            if (!failed.empty())
                message += ", " + std::to_string(failed.size()) + " diff(s) failed";
            refresh_data(true);
        } catch (const std::exception& e) {
            message = string("Error: ") + e.what();
        }
    }

    void delete_snapshots_ui() {
        std::vector<string> paths;
        long long total_size = 0;
        for (auto& key : selected) {
            for (auto& s : snapshots) {
                if (s.path == key) {
                    paths.push_back(s.path);
                    total_size += s.size;
                    break;
                }
            }
        }
        if (paths.empty()) return;

        size_t failed = 0;
        string first_error;
        for (auto& d : paths) {
            try {
                remove_tree(d);
            } catch (const std::exception& e) {
                if (failed == 0) first_error = e.what();
                ++failed;
            }
        }

        if (failed == 0) {
            string size_text = wide_to_utf8(format_size(total_size));
            if (delete_to_recycle())
                message = "Moved " + std::to_string(paths.size()) +
                          " snapshot(s) to the Recycle Bin (" + size_text + ")";
            else
                message = "Removed " + std::to_string(paths.size()) + " snapshot(s), freed " +
                          size_text;
        } else if (failed == paths.size()) {
            message = "Error: " + first_error;
        } else {
            string size_text = wide_to_utf8(format_size(total_size));
            message = "Removed " + std::to_string(paths.size() - failed) + " snapshot(s) (" +
                      size_text + "), " + std::to_string(failed) +
                      " failed: " + utf8_ellipsize(first_error, 40);
        }
        refresh_data(true);
    }

    // -----------------------------------------------------------------------
    // Drawing
    // -----------------------------------------------------------------------
    SortField current_sort_field() const {
        return tab == TAB_SESSIONS ? session_sort : snapshot_sort;
    }
    bool current_sort_desc() const {
        return tab == TAB_SESSIONS ? session_desc : snapshot_desc;
    }
    wstring sort_label() const {
        return utf8_to_wide(sort_field_name(current_sort_field()));
    }

    void draw() {
        erase();
        int h = 0, w = 0;
        getmaxyx(stdscr, h, w);
        if (w < 1) w = 1;
        if (h < 1) h = 1;

        draw_header(w);

        int list_start_y = 2;
        int list_end_y = h - 5;                 // cursor info + 2 help lines + message below
        if (list_end_y <= list_start_y) list_end_y = list_start_y + 1;
        int visible = list_end_y - list_start_y;
        page_rows = visible > 1 ? visible - 1 : 1;

        if (tab == TAB_SESSIONS) draw_session_list(list_start_y, list_end_y, w);
        else draw_snapshot_list(list_start_y, list_end_y, w);

        draw_info_line(h - 4, w);
        draw_help_lines(h - 3, w);              // occupies h-3 and h-2
        draw_message_line(h - 1, w);

        if (confirm_mode) draw_confirm_dialog(h, w);

        wnoutrefresh(stdscr);
        doupdate();
    }

    // Line 0: tabs, filter state and totals.
    void draw_header(int w) {
        wattrset(stdscr, A_NORMAL);
        mvwaddswstr(stdscr, 0, 0, wstring((size_t)w, L' '));

        wchar_t buf[128];
        swprintf(buf, 128, L" Sessions (%d) ", (int)sessions.size());
        wstring tab_s = buf;
        swprintf(buf, 128, L" Snapshots (%d) ", (int)snapshots.size());
        wstring tab_p = buf;

        int off = 0;
        struct TabLabel { const wstring& label; bool active; } tabs[2] = {
            {tab_s, tab == TAB_SESSIONS},
            {tab_p, tab == TAB_SNAPSHOTS}
        };
        for (auto& t : tabs) {
            wattrset(stdscr, t.active ? A_REVERSE : COLOR_PAIR(COLOR_DIM));
            mvwaddswstr(stdscr, 0, off, t.label);
            off += (int)t.label.size();
        }

        // right hand side: filter / sort / orphan totals
        size_t orphans = 0;
        long long orphan_bytes = 0;
        bool sizes_pending = false;
        for (auto& s : snapshots) {
            if (!s.active_sessions) {
                ++orphans;
                orphan_bytes += s.size;
            }
            if (!s.size_known) sizes_pending = true;
        }

        wstring right;
        const string& shown = active_filter();
        if (!shown.empty()) {
            wstring f = utf8_to_wide(shown);
            if (f.size() > 24) f = truncate(f, 24);
            right += L"filter:\"" + f + L"\" ";
        }
        swprintf(buf, 128, L"%d/%d  sort:%ls%ls  orphan:%d(%ls)%ls",
                 (int)visible_rows(), (int)total_rows(), sort_label().c_str(),
                 current_sort_desc() ? L"\u2193" : L"\u2191", (int)orphans,
                 format_size(orphan_bytes).c_str(),
                 sizes_pending ? L"  sizing..." : L"");
        right += buf;

        int x = w - (int)right.size();
        if (x > off) {
            wattrset(stdscr, shown.empty() ? COLOR_PAIR(COLOR_DIM) : COLOR_PAIR(COLOR_FILTER));
            mvwaddswstr(stdscr, 0, x, right);
        }
        wattrset(stdscr, A_NORMAL);
    }

    void draw_info_line(int y, int w) {
        if (y < 0 || y >= LINES) return;
        string info;
        if (tab == TAB_SESSIONS) {
            if (!view.empty()) {
                const Session& s = sessions[(size_t)view[(size_t)cursor]];
                info = "Session ID: " + s.id;
                if (!s.worktree.empty()) info += "   worktree: " + s.worktree;
            }
        } else {
            if (!view.empty()) {
                const Snapshot& s = snapshots[(size_t)view[(size_t)cursor]];
                info = "Snapshot: " + wide_to_utf8(s.name) + "   path: " + s.path;
                if (!s.worktree.empty()) info += "   worktree: " + s.worktree;
            }
        }
        wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
        mvwaddswstr(stdscr, y, 0, truncate(utf8_to_wide(info), (size_t)w));
        wattrset(stdscr, A_NORMAL);
    }

    void draw_help_lines(int y, int w) {
        if (y < 0 || y >= LINES) return;
        if (filter_mode) {
            WINDOW* win = derwin(stdscr, 1, w, y, 0);
            wbkgd(win, ' ' | COLOR_PAIR(COLOR_FILTER));
            wstring prompt = L" Filter: " + utf8_to_wide(filter_edit) + L"_";
            waddswstr(win, truncate(prompt, (size_t)w));
            wnoutrefresh(win);
            delwin(win);
        } else if (confirm_mode) {
            WINDOW* win = derwin(stdscr, 1, w, y, 0);
            wbkgd(win, ' ' | COLOR_PAIR(COLOR_HELP));
            waddswstr(win, truncate(L" Confirm remove?  [Y] Yes  [N] Cancel ", (size_t)w));
            wnoutrefresh(win);
            delwin(win);
        } else {
            wchar_t buf[160];
            swprintf(buf, 160,
                     L" up/down:Move  Space:Sel  ^A:All  ^D:None  /:Filter  s:Sort  "
                     L"d:Del  c:Copy  o:Jump  Tab:View  F5:Reload  q:Quit | Sel %d/%d ",
                     (int)selected.size(), (int)visible_rows());
            WINDOW* win = derwin(stdscr, 1, w, y, 0);
            wbkgd(win, ' ' | COLOR_PAIR(COLOR_HELP));
            waddswstr(win, truncate(buf, (size_t)w));
            wnoutrefresh(win);
            delwin(win);
        }

        // second help line: secondary keys / state
        if (y + 1 < LINES) {
            wchar_t buf2[160];
            swprintf(buf2, 160,
                     L" PgUp/PgDn:Page  Home/End:Jump  r:Reveal  O:Orphans  x:Clean diffs  "
                     L"^E:cleanup on exit:%ls ",
                     cleanup_on_exit ? L"ON" : L"OFF");
            wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
            mvwaddswstr(stdscr, y + 1, 0, truncate(buf2, (size_t)w));
            wattrset(stdscr, A_NORMAL);
        }
    }

    void draw_message_line(int y, int w) {
        if (y < 0 || y >= LINES || message.empty()) return;
        attr_t attr = COLOR_PAIR(message.find("Error") != string::npos ? COLOR_WARN : COLOR_DIM);
        wattrset(stdscr, attr);
        mvwaddswstr(stdscr, y, 0, truncate(utf8_to_wide(message), w > 0 ? (size_t)w - 1 : 0));
        wattrset(stdscr, A_NORMAL);
    }

    void adjust_scroll(int visible) {
        if (cursor < scroll) scroll = cursor;
        else if (cursor >= scroll + visible) scroll = cursor - visible + 1;
        if (scroll < 0) scroll = 0;
    }

    void draw_session_list(int start_y, int end_y, int w) {
        const int checkbox_w = 3, date_w = 13, project_w = 20, msgs_w = 6, col_gap = 1;
        if (view.empty()) {
            wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
            const char* why = sessions.empty()
                                  ? "No sessions found."
                                  : (filter.empty() ? "No sessions match the current filter."
                                                    : "No session matches the filter.");
            mvwaddstr(stdscr, start_y + 1, 2, why);
            return;
        }
        int visible = end_y - start_y;
        if (visible < 1) visible = 1;
        adjust_scroll(visible);

        int title_w = w - checkbox_w - col_gap - date_w - col_gap - project_w - col_gap - msgs_w - 2;
        if (title_w < 1) title_w = 1;

        for (int row = scroll; row < std::min((int)view.size(), scroll + visible); ++row) {
            int y = start_y + (row - scroll);
            if (y >= end_y) break;
            Session& s = sessions[(size_t)view[(size_t)row]];
            bool is_sel = selected.count(s.id) > 0;

            attr_t attr = is_sel ? COLOR_PAIR(COLOR_SELECTED) : COLOR_PAIR(COLOR_DIM);
            if (row == cursor) attr |= A_REVERSE;
            wattrset(stdscr, attr);

            mvwaddswstr(stdscr, y, 0, is_sel ? L"[x]" : L"[ ]");

            wstring title_text = truncate(s.title.empty() ? L"(untitled)" : s.title, (size_t)title_w);
            mvwaddswstr(stdscr, y, checkbox_w + col_gap, ljust(title_text, (size_t)title_w));

            int off = checkbox_w + col_gap + title_w + col_gap;
            mvwaddswstr(stdscr, y, off, ljust(format_time(s.time_created), (size_t)date_w));

            string proj = s.project_name.empty() ? s.worktree : s.project_name;
            if (proj.empty()) proj = "-";
            off += date_w + col_gap;
            mvwaddswstr(stdscr, y, off,
                        ljust(truncate(utf8_to_wide(proj), (size_t)project_w), (size_t)project_w));

            off += project_w + col_gap;
            mvwaddswstr(stdscr, y, off, rjust(std::to_wstring(s.msg_count), (size_t)msgs_w));
        }
    }

    void draw_snapshot_list(int start_y, int end_y, int w) {
        const int tag_w = 12, size_w = 8, name_w = 25, col_gap = 1;
        if (view.empty()) {
            wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
            const char* why = snapshots.empty() ? "No snapshots found."
                                                : "No snapshot matches the current filter.";
            mvwaddstr(stdscr, start_y + 1, 2, why);
            return;
        }
        int visible = end_y - start_y;
        if (visible < 1) visible = 1;
        adjust_scroll(visible);

        int path_w = w - tag_w - col_gap - size_w - col_gap - name_w - col_gap;
        if (path_w < 1) path_w = 1;

        for (int row = scroll; row < std::min((int)view.size(), scroll + visible); ++row) {
            int y = start_y + (row - scroll);
            if (y >= end_y) break;
            Snapshot& s = snapshots[(size_t)view[(size_t)row]];
            bool is_orphan = !s.active_sessions;
            bool is_sel = selected.count(s.path) > 0;

            attr_t attr;
            if (is_sel) attr = COLOR_PAIR(COLOR_SELECTED);
            else if (is_orphan) attr = COLOR_PAIR(COLOR_WARN);
            else attr = COLOR_PAIR(COLOR_DIM);
            if (row == cursor) attr |= A_REVERSE;
            wattrset(stdscr, attr);

            wstring checkbox = is_sel ? L"[x]" : L"[ ]";
            wstring tag = is_orphan ? L"[ORPHAN] " : L"[ACTIVE] ";
            mvwaddswstr(stdscr, y, 0, ljust(checkbox + tag, (size_t)tag_w));

            int off = tag_w + col_gap;
            mvwaddswstr(stdscr, y, off,
                        ljust(truncate(utf8_to_wide(s.worktree), (size_t)path_w), (size_t)path_w));

            off += path_w + col_gap;
            mvwaddswstr(stdscr, y, off,
                        rjust(s.size_known ? format_size(s.size) : wstring(L"..."),
                              (size_t)size_w));

            off += size_w + col_gap;
            mvwaddswstr(stdscr, y, off, ljust(truncate(s.name, (size_t)name_w), (size_t)name_w));
        }
    }

    string describe_key(const string& key) const {
        return tab == TAB_SESSIONS ? "session " + utf8_ellipsize(key, 24)
                                   : utf8_ellipsize(key, 52);
    }

    void draw_confirm_dialog(int h, int w) {
        int selected_count = (int)selected.size();
        long long total_size = 0;
        std::vector<string> listing(selected.begin(), selected.end());
        if (tab == TAB_SNAPSHOTS) {
            for (auto& s : snapshots)
                if (selected.count(s.path)) total_size += s.size;
        }

        const size_t max_listed = 6;
        int listed = (int)std::min(max_listed, listing.size());

        int dialog_w = 62;
        if (dialog_w > w - 2) dialog_w = w - 2;
        if (dialog_w < 24) dialog_w = 24;
        int dialog_h = 6 + listed + (listing.size() > max_listed ? 1 : 0);
        if (dialog_h > h - 2) dialog_h = h - 2;

        int dy = (h - dialog_h) / 2;
        int dx = (w - dialog_w) / 2;
        if (dy < 0) dy = 0;
        if (dx < 0) dx = 0;

        auto put = [&](int row, const wstring& text) {
            if (row < 0 || row >= dialog_h) return;
            mvwaddswstr(stdscr, dy + row, dx + 2, truncate(text, (size_t)dialog_w - 4));
        };

        wattrset(stdscr, COLOR_PAIR(COLOR_WARN) | A_BOLD);
        for (int y = dy; y < dy + dialog_h && y < h; y++)
            mvwaddswstr(stdscr, y, dx, wstring((size_t)dialog_w, L' '));
        mvwaddswstr(stdscr, dy, dx, L"\u250c" + wstring((size_t)dialog_w - 2, L'\u2500') + L"\u2510");
        mvwaddswstr(stdscr, dy + dialog_h - 1, dx,
                    L"\u2514" + wstring((size_t)dialog_w - 2, L'\u2500') + L"\u2518");
        for (int y = dy + 1; y < dy + dialog_h - 1 && y < h; y++) {
            mvwaddswstr(stdscr, y, dx, L"\u2502");
            mvwaddswstr(stdscr, y, dx + dialog_w - 1, L"\u2502");
        }

        wchar_t buf[128];
        swprintf(buf, 128, L" Remove %d item(s)?", selected_count);
        put(1, buf);

        wstring detail = tab == TAB_SNAPSHOTS
                             ? L"This permanently removes these snapshot folders ("
                                   + format_size(total_size) + L"):"
                             : L"These sessions and their messages are removed:";
        put(2, detail);

        for (int i = 0; i < listed; i++)
            put(3 + i, L"  - " + utf8_to_wide(describe_key(listing[(size_t)i])));
        if ((int)listing.size() > listed)
            put(3 + listed, L"  ... and " + std::to_wstring(listing.size() - (size_t)listed) +
                                L" more");

        put(dialog_h - 3, delete_to_recycle()
                              ? L" -> goes to the Recycle Bin (recoverable)"
                              : L" -> permanently deleted, this cannot be undone");
        put(dialog_h - 2, L"[Y] Confirm    [N] Cancel");
        wattrset(stdscr, A_NORMAL);
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
// Headless listing mode. It renders the very same pipeline the interactive UI
// uses (load -> filter -> sort -> format), which makes this both a scriptable
// feature and the only way to exercise the TUI logic without a console.
struct DumpOptions {
    bool enabled = false;
    bool snapshots = false;
    bool orphans_only = false;
    string filter;
    SortField field = SORT_CREATED;
    bool field_given = false;
    bool ascending = false;
    long long limit = 0;      // 0 = no limit
};

static bool parse_sort_field(const string& name, SortField* out) {
    for (int i = 0; i < SORT_FIELD_COUNT; ++i) {
        SortField f = (SortField)i;
        if (name == sort_field_name(f)) {
            *out = f;
            return true;
        }
    }
    return false;
}

static void print_usage() {
    printf("opencode-session-manager " OPENCODE_SM_VERSION_STRING " [options]\n"
           "\n"
           "Interactive TUI (no options opens the UI):\n"
           "  --cleanup            also clean up orphan session_diff files on exit\n"
           "  --no-recycle         delete permanently instead of using the Recycle Bin\n"
           "\n"
           "Headless listing (same filter/sort pipeline as the UI):\n"
           "  --list               print the current view and exit\n"
           "  --snapshots          list snapshot directories instead of sessions\n"
           "  --orphans            --snapshots: only directories no session references\n"
           "  --filter <text>      match id / title / project / worktree (or path / name)\n"
           "  --sort <field>       created|updated|msgs|size|status|id|name|project\n"
           "  --asc                sort ascending (default: descending)\n"
           "  --limit <n>          print at most n rows\n"
           "\n"
           "  --version            print the version and exit\n"
           "  --help               show this message\n");
}

static int run_list(const DumpOptions& o) {
    ViewQuery q;
    q.filter = o.filter;
    q.orphans_only = o.orphans_only;
    q.descending = !o.ascending;

    if (!o.snapshots) {
        q.field = o.field_given ? o.field : SORT_CREATED;
        std::vector<Session> sessions = load_sessions(DB_PATH);
        std::vector<int> view = build_session_view(sessions, q);

        long long shown = 0;
        for (int idx : view) {
            if (o.limit > 0 && shown >= o.limit) break;
            const Session& s = sessions[(size_t)idx];
            string proj = s.project_name.empty() ? s.worktree : s.project_name;
            // Note the explicit casts: MinGW's printf cannot print %lld.
            printf("%-36s %6d  %-12s  %-28s  %s\n", s.id.c_str(), (int)s.msg_count,
                   wide_to_utf8(format_time(s.time_created)).c_str(),
                   proj.empty() ? "-" : utf8_ellipsize(proj, 28).c_str(),
                   s.title.empty() ? "(untitled)" : utf8_ellipsize(wide_to_utf8(s.title), 60).c_str());
            ++shown;
        }
        printf("%d of %d session(s) shown   (sort: %s %s)\n", (int)shown, (int)view.size(),
               sort_field_name(q.field), q.descending ? "desc" : "asc");
        return 0;
    }

    q.field = o.field_given ? o.field : SORT_SIZE;
    std::vector<Snapshot> snaps = load_snapshots(DB_PATH, SNAPSHOT_DIR, /*with_sizes=*/true);
    std::vector<int> view = build_snapshot_view(snaps, q);

    long long shown = 0, total_bytes = 0;
    for (int idx : view) {
        const Snapshot& s = snaps[(size_t)idx];
        total_bytes += s.size;
        if (o.limit > 0 && shown >= o.limit) continue;
        printf("%-9s %-9s  %-44s  %s\n", s.active_sessions ? "in use" : "orphan",
               wide_to_utf8(format_size(s.size)).c_str(),
               utf8_ellipsize(s.worktree, 44).c_str(),
               wide_to_utf8(s.name).c_str());
        ++shown;
    }
    printf("%d of %d snapshot(s) shown, %s in total   (sort: %s %s)\n", (int)shown, (int)view.size(),
           wide_to_utf8(format_size(total_bytes)).c_str(), sort_field_name(q.field),
           q.descending ? "desc" : "asc");
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    bool cleanup_on_exit = false;
    DumpOptions dump;

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto need_value = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "Option %s needs a value.\n", what);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--list") {
            dump.enabled = true;
        } else if (a == "--snapshots") {
            dump.snapshots = true;
        } else if (a == "--orphans") {
            dump.orphans_only = true;
        } else if (a == "--asc") {
            dump.ascending = true;
        } else if (a == "--cleanup") {
            cleanup_on_exit = true;
        } else if (a == "--no-recycle") {
            set_delete_to_recycle(false);
        } else if (a == "--filter") {
            const char* v = need_value("--filter");
            if (!v) return 2;
            dump.filter = v;
        } else if (a == "--limit") {
            const char* v = need_value("--limit");
            if (!v) return 2;
            dump.limit = atoll(v);
        } else if (a == "--sort") {
            const char* v = need_value("--sort");
            if (!v) return 2;
            if (!parse_sort_field(v, &dump.field)) {
                fprintf(stderr, "Unknown sort field: %s\n", v);
                return 2;
            }
            dump.field_given = true;
        } else if (a == "--version" || a == "-V") {
            printf("opencode-session-manager %s\n", OPENCODE_SM_VERSION_STRING);
            return 0;
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", a.c_str());
            print_usage();
            return 2;
        }
    }

    if (!path_exists(DB_PATH)) {
        fprintf(stderr, "Database not found: %s\n", DB_PATH.c_str());
        fprintf(stderr, "Set OPENCODE_DATA_DIR to point at another data directory.\n");
        return 1;
    }

    if (dump.enabled) {
        try {
            return run_list(dump);
        } catch (const std::exception& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            return 1;
        }
    }

    initscr();
    try {
        App app;
        app.cleanup_on_exit = cleanup_on_exit;
        app.run();
    } catch (const std::exception&) {
        return 1;
    }

    // Opt-in only: silently rewriting the data directory on exit is not
    // something a cleanup tool should do behind the user's back.
    if (cleanup_on_exit) {
        try {
            std::vector<string> failed;
            auto orphans = cleanup_orphan_diffs(DB_PATH, DIFF_DIR, &failed);
            if (!orphans.empty())
                printf("Cleaned up %d orphan diff file(s)%s.\n", (int)orphans.size(),
                       delete_to_recycle() ? " (moved to the Recycle Bin)" : "");
            if (!failed.empty())
                printf("%d orphan diff file(s) could not be removed.\n", (int)failed.size());
        } catch (const std::exception& e) {
            printf("Orphan diff cleanup failed: %s\n", e.what());
        }
    }
    return 0;
}
