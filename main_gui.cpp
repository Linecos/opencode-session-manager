// OpenCode Session Manager - GUI (Dear ImGui + Win32 + OpenGL3)
// Desktop tool to browse old OpenCode sessions & snapshots clean stuff,
// with session-.-copy and one-click resume (jump into a session).
//
// No bundled DLLs: uses only Windows system libraries (opengl32.dll etc.).
// Build (MinGW-w64 + sqlite3 from C:\MinGW\opt):
//   g++ -std=c++17 -O2 -I imgui -I imgui/backends
//        -I "C:\MinGW\opt\include"
//        main_gui.cpp opencode_data.cpp
//        imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp
//        imgui/imgui_widgets.cpp
//        imgui/backends/imgui_impl_win32.cpp imgui/backends/imgui_impl_opengl3.cpp
//        -L "C:\MinGW\opt\lib" -lsqlite3 -lopengl32 -ldwmapi -o opencode-session-manager-gui.exe
//
// Optional env override for data location (handy for testing):
//   OPENCODE_DATA_DIR=<dir containing opencode.db, storage/, snapshot/>

#define WIN32_LEAN_AND_MEAN 1
#define NCURSES_WIDECHAR 1  // keep this out of the way (harmless)

#include <windows.h>
#include <tchar.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#include <GL/gl.h>
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
static string g_error;
static bool g_cleanup_on_exit = true;
static bool g_confirm_delete = false;

static void refresh_data(bool keep_selection) {
    g_sessions = load_sessions(GUI_DB_PATH);
    g_snapshots = load_snapshots(GUI_DB_PATH, GUI_SNAPSHOT_DIR);
    if (!keep_selection) {
        g_selected.clear();
        g_message.clear();
        g_error.clear();
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
            g_selected.clear();
        } catch (const std::exception& e) {
            g_message = string("Error: ") + e.what();
            g_error = g_message;
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
            g_selected.clear();
        } catch (const std::exception& e) {
            g_message = string("Error: ") + e.what();
            g_error = g_message;
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
// UI: in-app error modal (shown when a delete fails; not a Windows MessageBox)
// ---------------------------------------------------------------------------
static void draw_error_modal() {
    if (g_error.empty()) return;
    ImGui::OpenPopup("Error");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool open = true;
    if (ImGui::BeginPopupModal("Error", &open,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", g_error.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(140, 0))) {
            g_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!open) g_error.clear();
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
    ImGui::Spacing();
    bool is_err = g_message.rfind("Error", 0) == 0;
    if (is_err)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", g_message.c_str());
    else
        ImGui::TextUnformatted(g_message.empty() ? "Ready." : g_message.c_str());

    draw_confirm_modal();
    draw_error_modal();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Win32 window + WGL (OpenGL 3.0 core) plumbing
// ---------------------------------------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND g_hwnd = nullptr;
static HDC  g_hdc  = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;           // don't flicker; we repaint every frame
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// WGL_ARB_create_context constants (avoid dragging in <GL/wglext.h>)
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB         0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef HGLRC (WINAPI* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
static PFN_wglCreateContextAttribsARB wglCreateContextAttribsARB = nullptr;

static HGLRC create_gl_context(HWND hwnd) {
    g_hdc = GetDC(hwnd);
    if (!g_hdc)
        return nullptr;

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 24;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    const int pixel_format = ChoosePixelFormat(g_hdc, &pfd);
    if (pixel_format == 0 || !SetPixelFormat(g_hdc, pixel_format, &pfd))
        return nullptr;

    // probe wglCreateContextAttribsARB via a temporary 1.1 context
    HGLRC probe = wglCreateContext(g_hdc);
    if (!probe)
        return nullptr;
    wglMakeCurrent(g_hdc, probe);
    wglCreateContextAttribsARB =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");

    HGLRC gl_context = nullptr;
    if (wglCreateContextAttribsARB) {
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 0,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            WGL_CONTEXT_FLAGS_ARB,         0,
            0
        };
        gl_context = wglCreateContextAttribsARB(g_hdc, nullptr, attribs);
    }
    if (gl_context == nullptr)
        gl_context = wglCreateContext(g_hdc);      // fallback: legacy GL 1.x

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(probe);

    if (gl_context == nullptr || !wglMakeCurrent(g_hdc, gl_context)) {
        if (gl_context)
            wglDeleteContext(gl_context);
        return nullptr;
    }
    return gl_context;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Per-monitor DPI awareness (best font / OpenGL scaling on HiDPI).
    ImGui_ImplWin32_EnableDpiAwareness();

    if (!path_exists(GUI_DB_PATH)) {
        fprintf(stderr, "Database not found: %s\n", GUI_DB_PATH.c_str());
        MessageBoxA(nullptr, ("Database not found:\n" + GUI_DB_PATH).c_str(),
                    "OpenCode Session Manager", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"OpenCodeSessionManagerWnd";
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "Error: RegisterClassExW() failed.\n");
        return 1;
    }

    HWND window = CreateWindowExW(0, wc.lpszClassName, L"OpenCode Session Manager",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
                                  nullptr, nullptr, hInstance, nullptr);
    if (window == nullptr) {
        fprintf(stderr, "Error: CreateWindowExW() failed.\n");
        return 1;
    }
    g_hwnd = window;

    HGLRC gl_context = create_gl_context(window);
    if (gl_context == nullptr) {
        fprintf(stderr, "Error: could not create OpenGL context.\n");
        return 1;
    }

    float main_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    if (main_scale <= 0.0f)
        main_scale = 1.0f;

    ShowWindow(window, nCmdShow);
    SetWindowPos(window, nullptr, 0, 0, (int)(1100 * main_scale), (int)(720 * main_scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

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

    ImGui_ImplWin32_InitForOpenGL(window);
    ImGui_ImplOpenGL3_Init("#version 130");

    refresh_data(false);
    ImVec4 clear_color = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (IsIconic(window)) {
            Sleep(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        draw_ui();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(g_hdc);
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
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(gl_context);
    ReleaseDC(window, g_hdc);
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}