// OpenCode Session Manager -- C++ port of main.py
// TUI tool to visually browse and clean old OpenCode conversations & snapshot orphans.
//
// Build (MinGW-w64, ncursesw + sqlite3 from C:\MinGW\opt), one line:
//   g++ -std=c++17 -O2 -DNCURSES_WIDECHAR -I"C:\MinGW\opt\include" main.cpp opencode_data.cpp
//        -L"C:\MinGW\opt\lib" -lncursesw -lsqlite3 -o opencode-session-manager.exe
//
// Optional env override for data location (handy for testing):
//   OPENCODE_DATA_DIR=<dir containing opencode.db, storage/, snapshot/>
//
// Key bindings:
//   up/down, j/k   move cursor          Space        toggle selection
//   Ctrl+A         select all           Ctrl+D       clear selection
//   Tab            switch view          Enter        delete selected (confirm)
//   c              copy resume command (opencode -s <id>)
//   o              open/resume session in a new console
//   Q / Esc        quit

#define NCURSES_WIDECHAR 1
#include <ncursesw/ncurses.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "opencode_data.hpp"

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
    COLOR_TAB_INACTIVE
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

#ifdef _WIN32
// Put UTF-16 text on the Windows clipboard (so pasting into a terminal works).
static void copy_to_clipboard(const string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    wstring wtext = utf8_to_wide(text);
    SIZE_T bytes = (wtext.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        void* dst = GlobalLock(hg);
        if (dst) {
            memcpy(dst, wtext.c_str(), bytes);
            GlobalUnlock(hg);
            SetClipboardData(CF_UNICODETEXT, hg);
        }
    }
    CloseClipboard();
}

// Resume a session: open a new console in the session's worktree (if it still
// exists) and run `opencode -s <session_id>` inside it.
static bool launch_opencode_session(const string& session_id, const string& worktree) {
    wstring cmd = L"cmd.exe /c opencode -s \"" + utf8_to_wide(session_id) + L"\"";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;
    ZeroMemory(&pi, sizeof(pi));

    wstring wd;
    if (is_dir(worktree)) wd = utf8_to_wide(worktree);

    BOOL ok = CreateProcessW(
        nullptr,                      // application name
        &cmdline[0],                  // command line
        nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE,           // give it its own console window
        nullptr,
        wd.empty() ? nullptr : wd.c_str(),
        &si, &pi);

    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    return false;
}
#else
static bool launch_opencode_session(const string&, const string&) { return false; }
#endif

// ---------------------------------------------------------------------------
// TUI application
// ---------------------------------------------------------------------------
static const int ACTIVE_TAB_SESSIONS = 0;
static const int ACTIVE_TAB_SNAPSHOTS = 1;

static const string DB_PATH = db_path();
static const string DIFF_DIR = diff_dir();
static const string SNAPSHOT_DIR = snapshot_dir();

class App {
public:
    int tab = ACTIVE_TAB_SESSIONS;
    std::vector<Session> sessions;
    std::vector<Snapshot> snapshots;
    std::set<int> selected;
    int cursor = 0;
    int scroll = 0;
    bool confirm_mode = false;
    string message;
    bool running = true;

    void run() {
        init_tui();
        refresh_data(false);
        while (running) {
            draw();
            int key = getch();
            handle_key(key);
        }
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
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
    }

    void refresh_data(bool keep_selection) {
        sessions = load_sessions(DB_PATH);
        snapshots = load_snapshots(DB_PATH, SNAPSHOT_DIR);
        if (!keep_selection) {
            selected.clear();
            cursor = 0;
            scroll = 0;
        }
    }

    size_t list_size() const {
        return tab == ACTIVE_TAB_SESSIONS ? sessions.size() : snapshots.size();
    }

    void handle_key(int key) {
        if (confirm_mode) {
            if (key == 'y' || key == 'Y') do_delete();
            confirm_mode = false;
            return;
        }

        if (key == 'q' || key == 27) running = false;
        else if (key == '\t') {
            tab = 1 - tab;
            selected.clear();
            cursor = 0;
            scroll = 0;
            message.clear();
        } else if (key == KEY_UP || key == 'k') {
            if (cursor > 0) cursor--;
        } else if (key == KEY_DOWN || key == 'j') {
            if (cursor < (int)list_size() - 1) cursor++;
        } else if (key == ' ') {
            if (selected.count(cursor)) selected.erase(cursor);
            else selected.insert(cursor);
        } else if (key == 1) {                   // Ctrl+A
            selected.clear();
            for (int i = 0; i < (int)list_size(); i++) selected.insert(i);
        } else if (key == 4) {                   // Ctrl+D
            selected.clear();
        } else if (key == 'c' || key == 'C') {
            copy_command();
        } else if (key == 'o' || key == 'O') {
            jump_session();
        } else if (key == 10 || key == 13 || key == KEY_ENTER) {
            if (!selected.empty()) confirm_mode = true;
        }
    }

    void copy_command() {
        if (tab == ACTIVE_TAB_SESSIONS) {
            if (sessions.empty()) return;
            string cmd = "opencode -s " + sessions[cursor].id;
            copy_to_clipboard(cmd);
            message = "Copied: " + cmd;
        } else {
            message = "Resume command copy is only available for sessions.";
        }
    }

    void jump_session() {
        if (tab == ACTIVE_TAB_SESSIONS) {
            if (sessions.empty()) return;
            const Session& s = sessions[cursor];
            if (launch_opencode_session(s.id, s.worktree))
                message = "Resumed session " + s.id;
            else
                message = "Failed to launch opencode";
        } else {
            message = "Jump is only available for sessions.";
        }
    }

    void do_delete() {
        if (tab == ACTIVE_TAB_SESSIONS) delete_sessions_ui();
        else delete_snapshots_ui();
    }

    void delete_sessions_ui() {
        std::vector<string> ids;
        for (int i : selected) ids.push_back(sessions[i].id);
        if (ids.empty()) return;
        try {
            int n = delete_sessions(DB_PATH, ids);
            auto orphans = cleanup_orphan_diffs(DB_PATH, DIFF_DIR);
            message = "Deleted " + std::to_string(n) + " session(s)";
            if (!orphans.empty())
                message += ", cleaned " + std::to_string(orphans.size()) + " orphan diff(s)";
            refresh_data(true);
        } catch (const std::exception& e) {
            message = string("Error: ") + e.what();
        }
    }

    void delete_snapshots_ui() {
        std::vector<string> dirs;
        long long total_size = 0;
        for (int i : selected) {
            dirs.push_back(snapshots[i].path);
            total_size += snapshots[i].size;
        }
        if (dirs.empty()) return;
        try {
            for (auto& d : dirs) remove_tree(d);
            message = "Removed " + std::to_string(dirs.size()) + " snapshot(s), freed ";
            message += wide_to_utf8(format_size(total_size));
            refresh_data(true);
        } catch (const std::exception& e) {
            message = string("Error: ") + e.what();
        }
    }

    void draw() {
        erase();
        int h = 0, w = 0;
        getmaxyx(stdscr, h, w);
        if (w < 1) w = 1;
        if (h < 1) h = 1;

        draw_tab_bar();

        int list_start_y = 2;
        int list_end_y = h - 4;

        if (tab == ACTIVE_TAB_SESSIONS) draw_session_list(list_start_y, list_end_y, w);
        else draw_snapshot_list(list_start_y, list_end_y, w);

        // current-row info line (always shows the id / command of the cursor row)
        int info_y = h - 3;
        if (tab == ACTIVE_TAB_SESSIONS) {
            if (!sessions.empty()) {
                const Session& s = sessions[cursor];
                string info = "Session ID: " + s.id;
                wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
                mvwaddswstr(stdscr, info_y, 0, truncate(utf8_to_wide(info), w > 0 ? (size_t)w : 0));
                wattrset(stdscr, A_NORMAL);
            }
        } else {
            if (!snapshots.empty()) {
                const Snapshot& s = snapshots[cursor];
                string info = "Snapshot: " + wide_to_utf8(s.name) + "  |  worktree: " + s.worktree;
                wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
                mvwaddswstr(stdscr, info_y, 0, truncate(utf8_to_wide(info), w > 0 ? (size_t)w : 0));
                wattrset(stdscr, A_NORMAL);
            }
        }

        // help bar
        int help_y = h - 2;
        WINDOW* help_win = derwin(stdscr, 1, w, help_y, 0);
        wbkgd(help_win, ' ' | COLOR_PAIR(COLOR_HELP));
        wstring help_text;
        if (confirm_mode) {
            help_text = L" Confirm delete?  [Y] Yes  [N] No  ";
        } else {
            wchar_t hb[64];
            swprintf(hb, 64, L" Selected: %d/%d ", (int)selected.size(), (int)list_size());
            help_text = L" " + wstring(L"up/down:Move  Space:Select  Ctrl+A:All  Ctrl+D:None  "
                                      L"c:Copy  o:Jump  Tab:Switch View  Enter:Delete  Q:Quit  |") + hb;
        }
        waddswstr(help_win, truncate(help_text, (size_t)w));
        wnoutrefresh(help_win);
        delwin(help_win);

        // message line
        int msg_y = h - 1;
        if (!message.empty()) {
            attr_t attr = COLOR_PAIR(message.find("Error") != string::npos ? COLOR_WARN : COLOR_DIM);
            wattrset(stdscr, attr);
            mvwaddswstr(stdscr, msg_y, 0, truncate(utf8_to_wide(message), w > 0 ? (size_t)w - 1 : 0));
            wattrset(stdscr, A_NORMAL);
        }

        if (confirm_mode) draw_confirm_dialog(h, w);

        wnoutrefresh(stdscr);
        doupdate();
    }

    void draw_tab_bar() {
        int h = 0, w = 0;
        getmaxyx(stdscr, h, w);
        (void)h;

        wattrset(stdscr, A_NORMAL);
        mvwaddswstr(stdscr, 0, 0, wstring((size_t)w, L' '));

        wchar_t buf[64];
        swprintf(buf, 64, L" Sessions (%d) ", (int)sessions.size());
        wstring tab_s = buf;
        swprintf(buf, 64, L" Snapshots (%d) ", (int)snapshots.size());
        wstring tab_p = buf;

        int off = 0;
        struct TabLabel { const wstring& label; bool active; } tabs[2] = {
            {tab_s, tab == ACTIVE_TAB_SESSIONS},
            {tab_p, tab == ACTIVE_TAB_SNAPSHOTS}
        };
        for (auto& t : tabs) {
            wattrset(stdscr, t.active ? A_REVERSE : A_NORMAL);
            mvwaddswstr(stdscr, 0, off, t.label);
            off += (int)t.label.size();
        }
        if (off < w - 1) {
            wattrset(stdscr, A_NORMAL);
            mvwaddswstr(stdscr, 0, off, wstring((size_t)(w - off - 1), L' '));
        }
        wattrset(stdscr, A_NORMAL);
    }

    void adjust_scroll(int visible) {
        if (cursor < scroll) scroll = cursor;
        else if (cursor >= scroll + visible) scroll = cursor - visible + 1;
    }

    void draw_session_list(int start_y, int end_y, int w) {
        const int checkbox_w = 3, date_w = 13, project_w = 20, msgs_w = 6, col_gap = 1;
        if (sessions.empty()) {
            wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
            mvwaddstr(stdscr, start_y + 1, 2, "No sessions found.");
            return;
        }
        int visible = end_y - start_y;
        if (visible < 1) visible = 1;
        adjust_scroll(visible);

        int title_w = w - checkbox_w - col_gap - date_w - col_gap - project_w - col_gap - msgs_w - 2;
        if (title_w < 1) title_w = 1;

        for (int i = scroll; i < std::min((int)sessions.size(), scroll + visible); ++i) {
            int y = start_y + (i - scroll);
            Session& s = sessions[i];

            attr_t attr = selected.count(i) ? COLOR_PAIR(COLOR_SELECTED) : COLOR_PAIR(COLOR_DIM);
            if (i == cursor) attr |= A_REVERSE;
            wattrset(stdscr, attr);

            mvwaddswstr(stdscr, y, 0, selected.count(i) ? L"[x]" : L"[ ]");

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
        if (snapshots.empty()) {
            wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
            mvwaddstr(stdscr, start_y + 1, 2, "No snapshots found.");
            return;
        }
        int visible = end_y - start_y;
        if (visible < 1) visible = 1;
        adjust_scroll(visible);

        int path_w = w - tag_w - col_gap - size_w - col_gap - name_w - col_gap;
        if (path_w < 1) path_w = 1;

        for (int i = scroll; i < std::min((int)snapshots.size(), scroll + visible); ++i) {
            int y = start_y + (i - scroll);
            Snapshot& s = snapshots[i];
            bool is_orphan = !s.active_sessions;

            attr_t attr;
            if (is_orphan && !selected.count(i)) attr = COLOR_PAIR(COLOR_WARN);
            else if (selected.count(i)) attr = COLOR_PAIR(COLOR_SELECTED);
            else attr = COLOR_PAIR(COLOR_DIM);
            if (i == cursor) attr |= A_REVERSE;
            wattrset(stdscr, attr);

            wstring checkbox = selected.count(i) ? L"[x]" : L"[ ]";
            wstring tag = is_orphan ? L"[ORPHAN] " : L"[ACTIVE] ";
            mvwaddswstr(stdscr, y, 0, ljust(checkbox + tag, (size_t)tag_w));

            int off = tag_w + col_gap;
            mvwaddswstr(stdscr, y, off,
                        ljust(truncate(utf8_to_wide(s.worktree), (size_t)path_w), (size_t)path_w));

            off += path_w + col_gap;
            mvwaddswstr(stdscr, y, off, rjust(format_size(s.size), (size_t)size_w));

            off += size_w + col_gap;
            mvwaddswstr(stdscr, y, off, ljust(truncate(s.name, (size_t)name_w), (size_t)name_w));
        }
    }

    void draw_confirm_dialog(int h, int w) {
        int selected_count = (int)selected.size();
        long long total_size = 0;
        wchar_t buf[96];
        wstring detail;
        if (tab == ACTIVE_TAB_SNAPSHOTS) {
            for (int i : selected) total_size += snapshots[i].size;
            swprintf(buf, 96, L"freed %ls", format_size(total_size).c_str());
            detail = buf;
        } else {
            swprintf(buf, 96, L"%d session(s)", selected_count);
            detail = buf;
        }

        int dialog_w = 55, dialog_h = 5;
        int dy = (h - dialog_h) / 2;
        int dx = (w - dialog_w) / 2;
        if (dy < 0) dy = 0;
        if (dx < 0) dx = 0;

        wattrset(stdscr, COLOR_PAIR(COLOR_WARN) | A_BOLD);
        for (int y = dy; y < dy + dialog_h; y++)
            mvwaddswstr(stdscr, y, dx, wstring((size_t)dialog_w, L' '));
        mvwaddswstr(stdscr, dy, dx, L"\u250c" + wstring((size_t)dialog_w - 2, L'\u2500') + L"\u2510");
        mvwaddswstr(stdscr, dy + dialog_h - 1, dx, L"\u2514" + wstring((size_t)dialog_w - 2, L'\u2500') + L"\u2518");
        for (int y = dy + 1; y < dy + dialog_h - 1; y++) {
            mvwaddswstr(stdscr, y, dx, L"\u2502");
            mvwaddswstr(stdscr, y, dx + dialog_w - 1, L"\u2502");
        }
        swprintf(buf, 96, L" Delete %d item(s)?", selected_count);
        mvwaddswstr(stdscr, dy + 1, dx + 2, buf);
        mvwaddswstr(stdscr, dy + 2, dx + 2, L" This will permanently remove " + detail + L".");
        mvwaddswstr(stdscr, dy + 3, dx + 4, L"[Y] Confirm    [N] Cancel");
        wattrset(stdscr, A_NORMAL);
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const string DB_PATH = db_path();
    const string DIFF_DIR = diff_dir();
    if (!path_exists(DB_PATH)) {
        fprintf(stderr, "Database not found: %s\n", DB_PATH.c_str());
        return 1;
    }

    initscr();
    App app;
    app.run();
    endwin();

    try {
        auto orphans = cleanup_orphan_diffs(DB_PATH, DIFF_DIR);
        if (!orphans.empty())
            printf("Cleaned up %d orphan diff file(s).\n", (int)orphans.size());
    } catch (const std::exception&) {
        // best-effort post-run cleanup
    }
    return 0;
}