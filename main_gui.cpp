// OpenCode Session Manager -- GUI (Dear ImGui + SDL3 + OpenGL3)
// Desktop tool to browse and clean old OpenCode conversations & snapshot orphans,
// with session-id copy and one-click resume (jump into a session).
//
// Build (MinGW-w64 + sqlite3 from C:\MinGW\opt + bundled SDL3):
//   g++ -std=c++17 -O2 -I imgui -I imgui/backends -I third_party/SDL3/include
//        -I "C:\MinGW\opt\include"
//        main_gui.cpp opencode_data.cpp
//        imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp
//        imgui/imgui_widgets.cpp
//        imgui/backends/imgui_impl_sdl3.cpp imgui/backends/imgui_impl_opengl3.cpp
//        -L third_party/SDL3/lib -lSDL3 -lopengl32 -L "C:\MinGW\opt\lib" -lsqlite3
//        -o opencode-session-manager-gui.exe
//
// Optional env override for data location (handy for testing):
//   OPENCODE_DATA_DIR=<dir containing opencode.db, storage/, snapshot/>

#define NCURSES_WIDECHAR 1   // keep this out of the way (harmless)

#ifdef _WIN32
#include <windows.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "opencode_data.hpp"

using std::string;
using std::wstring;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const string GUI_DB_PATH = db_path();
static const string GUI_DIFF_DIR = diff_dir();
static const string GUI_SNAPSHOT_DIR = snapshot_dir();

static string u8(const wstring& w) { return wide_to_utf8(w); }
static string u8fmt_time(long long ts) { return u8(format_time(ts)); }
static string u8fmt_size(long long sz) { return u8(format_size(sz)); }

// ---------------------------------------------------------------------------
// Resume a session: open a new console in the session's worktree (if it still
// exists) and run `opencode -s <session_id>` inside it.
// ---------------------------------------------------------------------------
static bool launch_opencode_session(const string& session_id, const string& worktree) {
#ifdef _WIN32
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
#endif
    return false;
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
static const int TAB_SESSIONS = 0;
static const int TAB_SNAPSHOTS = 1;

static int g_tab = TAB_SESSIONS;
static std::vector<Session> g_sessions;
static std::vector<Snapshot> g_snapshots;
static std::set<int> g_selected;
static string g_message;
static bool g_cleanup_on_exit = true;
static bool g_confirm_delete = false;

static void refresh_data(bool keep_selection) {
    g_sessions = load_sessions(GUI_DB_PATH);
    g_snapshots = load_snapshots(GUI_DB_PATH, GUI_SNAPSHOT_DIR);
    if (!keep_selection) {
        g_selected.clear();
        g_message.clear();
    }
}

static size_t list_size() {
    return g_tab == TAB_SESSIONS ? g_sessions.size() : g_snapshots.size();
}

static void select_all() {
    g_selected.clear();
    for (size_t i = 0; i < list_size(); i++) g_selected.insert((int)i);
}

static void clear_selection() { g_selected.clear(); }

// ---------------------------------------------------------------------------
// Delete actions (shared with TUI semantics)
// ---------------------------------------------------------------------------
static void do_delete() {
    if (g_tab == TAB_SESSIONS) {
        std::vector<string> ids;
        for (int i : g_selected) ids.push_back(g_sessions[i].id);
        if (ids.empty()) return;
        try {
            int n = delete_sessions(GUI_DB_PATH, ids);
            auto orphans = cleanup_orphan_diffs(GUI_DB_PATH, GUI_DIFF_DIR);
            g_message = "Deleted " + std::to_string(n) + " session(s)";
            if (!orphans.empty())
                g_message += ", cleaned " + std::to_string(orphans.size()) + " orphan diff(s)";
            refresh_data(true);
        } catch (const std::exception& e) {
            g_message = string("Error: ") + e.what();
        }
    } else {
        std::vector<string> dirs;
        long long total_size = 0;
        for (int i : g_selected) {
            dirs.push_back(g_snapshots[i].path);
            total_size += g_snapshots[i].size;
        }
        if (dirs.empty()) return;
        try {
            for (auto& d : dirs) remove_tree(d);
            g_message = "Removed " + std::to_string(dirs.size()) + " snapshot(s), freed ";
            g_message += u8fmt_size(total_size);
            refresh_data(true);
        } catch (const std::exception& e) {
            g_message = string("Error: ") + e.what();
        }
    }
}

// ---------------------------------------------------------------------------
// UI: toolbar
// ---------------------------------------------------------------------------
static void draw_toolbar() {
    if (ImGui::Button("Refresh")) {
        refresh_data(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Select All")) {
        select_all();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Sel")) {
        clear_selection();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Cleanup orphan diffs on exit", &g_cleanup_on_exit);
    ImGui::SameLine();
    if (ImGui::Button("Delete Selected") && !g_selected.empty()) {
        g_confirm_delete = true;
    }
    if (ImGui::IsItemHovered() && g_selected.empty())
        ImGui::SetTooltip("Select items first");

    ImGui::SameLine();
    ImGui::Text("Selected: %d/%d", (int)g_selected.size(), (int)list_size());
}

// ---------------------------------------------------------------------------
// UI: sessions tab
// ---------------------------------------------------------------------------
static void draw_sessions_tab() {
    if (g_sessions.empty()) {
        ImGui::Text("No sessions found.");
        return;
    }

    ImGui::TextDisabled("Click [copy] to copy a session id, [jump] to resume it in opencode.");
    ImGui::Spacing();

    if (ImGui::BeginTable("sessions", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Sel",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Session", ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 3.2f);
        ImGui::TableSetupColumn("Project / Worktree", ImGuiTableColumnFlags_WidthStretch, 2.8f);
        ImGui::TableSetupColumn("Msgs",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < g_sessions.size(); i++) {
            Session& s = g_sessions[i];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool sel = g_selected.count((int)i) > 0;
            char cid[32];
            snprintf(cid, sizeof(cid), "##sel%zu", i);
            if (ImGui::Checkbox(cid, &sel)) {
                if (sel) g_selected.insert((int)i);
                else g_selected.erase((int)i);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.id.c_str());

            ImGui::TableSetColumnIndex(2);
            char bid[64];
            snprintf(bid, sizeof(bid), "copy##c%zu", i);
            if (ImGui::SmallButton(bid)) {
                ImGui::SetClipboardText(s.id.c_str());
                g_message = "Copied session id to clipboard";
            }
            ImGui::SameLine();
            snprintf(bid, sizeof(bid), "jump##j%zu", i);
            if (ImGui::SmallButton(bid)) {
                if (launch_opencode_session(s.id, s.worktree))
                    g_message = "Resumed session " + s.id;
                else
                    g_message = "Failed to launch opencode";
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open this session in opencode");

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(u8(s.title.empty() ? L"(untitled)" : s.title).c_str());

            ImGui::TableSetColumnIndex(4);
            string proj = s.project_name.empty() ? s.worktree : s.project_name;
            if (proj.empty()) proj = "-";
            ImGui::TextUnformatted(proj.c_str());

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%lld", s.msg_count);

            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(u8fmt_time(s.time_created).c_str());
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// UI: snapshots tab
// ---------------------------------------------------------------------------
static void draw_snapshots_tab() {
    if (g_snapshots.empty()) {
        ImGui::Text("No snapshots found.");
        return;
    }

    if (ImGui::BeginTable("snapshots", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Sel",    ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Worktree", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < g_snapshots.size(); i++) {
            Snapshot& s = g_snapshots[i];
            bool is_orphan = !s.active_sessions;
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool sel = g_selected.count((int)i) > 0;
            char cid[32];
            snprintf(cid, sizeof(cid), "##sel%zu", i);
            if (ImGui::Checkbox(cid, &sel)) {
                if (sel) g_selected.insert((int)i);
                else g_selected.erase((int)i);
            }

            ImGui::TableSetColumnIndex(1);
            if (is_orphan) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[ORPHAN]");
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "[ACTIVE]");
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(s.worktree.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(u8fmt_size(s.size).c_str());

            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(u8(s.name).c_str());
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// UI: confirmation modal
// ---------------------------------------------------------------------------
static void draw_confirm_modal() {
    if (!g_confirm_delete) return;
    ImGui::OpenPopup("Confirm Delete");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Confirm Delete", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        long long total_size = 0;
        string detail;
        if (g_tab == TAB_SNAPSHOTS) {
            for (int i : g_selected) total_size += g_snapshots[i].size;
            detail = "This will permanently remove " + std::to_string(g_selected.size()) +
                     " snapshot(s) and free " + u8fmt_size(total_size) + ".";
        } else {
            detail = "This will permanently remove " + std::to_string(g_selected.size()) +
                     " session(s).";
        }
        ImGui::Text("Delete %d item(s)?", (int)g_selected.size());
        ImGui::TextWrapped("%s", detail.c_str());
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                           "This cannot be undone.");

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Confirm Delete", ImVec2(140, 0))) {
            g_confirm_delete = false;
            do_delete();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140, 0))) {
            g_confirm_delete = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// UI: main window
// ---------------------------------------------------------------------------
static void draw_ui() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("OpenCode Session Manager", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        ImGui::TextUnformatted("View:");
        ImGui::Separator();
        if (ImGui::Button(g_tab == TAB_SESSIONS ? "[Sessions]" : "Sessions")) {
            g_tab = TAB_SESSIONS;
            g_selected.clear();
        }
        ImGui::Separator();
        if (ImGui::Button(g_tab == TAB_SNAPSHOTS ? "[Snapshots]" : "Snapshots")) {
            g_tab = TAB_SNAPSHOTS;
            g_selected.clear();
        }
        ImGui::SameLine(0, 40);
        ImGui::TextDisabled("Sessions: %d    Snapshots: %d",
                            (int)g_sessions.size(), (int)g_snapshots.size());
        ImGui::EndMenuBar();
    }

    draw_toolbar();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_tab == TAB_SESSIONS) draw_sessions_tab();
    else draw_snapshots_tab();

    // Status line
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(g_message.empty() ? "Ready." : g_message.c_str());

    draw_confirm_modal();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int, char**) {
    if (!path_exists(GUI_DB_PATH)) {
        fprintf(stderr, "Database not found: %s\n", GUI_DB_PATH.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenCode Session Manager",
                                 ("Database not found:\n" + GUI_DB_PATH).c_str(), nullptr);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }

    // GL 3.0 + GLSL 130 (portable, matches imgui_impl_opengl3 defaults)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow("OpenCode Session Manager",
                                          (int)(1100 * main_scale), (int)(720 * main_scale),
                                          window_flags);
    if (window == nullptr) {
        fprintf(stderr, "Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (gl_context == nullptr) {
        fprintf(stderr, "Error: SDL_GL_CreateContext(): %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;   // don't persist window layout

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    // Load a CJK-capable font (session titles / project names may be Chinese).
    const char* font_candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",     // Microsoft YaHei
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",   // SimHei
        "C:/Windows/Fonts/simsun.ttc",   // SimSun
        "C:/Windows/Fonts/simkai.ttf",   // KaiTi
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    ImFont* cjk_font = nullptr;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);
    for (auto* path : font_candidates) {
        cjk_font = io.Fonts->AddFontFromFileTTF(path, 16.0f * main_scale, &cfg, ranges.Data);
        if (cjk_font) break;
    }
    if (!cjk_font)
        io.Fonts->AddFontDefault();

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    refresh_data(false);
    ImVec4 clear_color = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        draw_ui();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Exit cleanup (best-effort), mirrors TUI behaviour.
    if (g_cleanup_on_exit) {
        try {
            auto orphans = cleanup_orphan_diffs(GUI_DB_PATH, GUI_DIFF_DIR);
            if (!orphans.empty()) {
                fprintf(stderr, "Cleaned up %d orphan diff file(s).\n", (int)orphans.size());
            }
        } catch (const std::exception&) {
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}