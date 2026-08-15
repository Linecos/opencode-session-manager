# OpenCode Session Manager

A desktop tool to visually browse and clean old [OpenCode](https://opencode.ai) conversations and orphan snapshot diffs. Ships as both a **terminal (TUI)** and a **GUI** application, written in C++17 for Windows.

## Features

- Browse all OpenCode sessions and snapshots in a table.
- Switch between **Sessions** and **Snapshots** views.
- Select multiple items, then delete them (with a confirmation dialog).
- **GUI extras**:
  - Copy the resume command `opencode -s <id>` to the clipboard with one click.
  - **Jump** button — resumes a session in a new console via `opencode -s <id>` (in its worktree if it still exists).
  - Chinese/CJK font support (loads Microsoft YaHei etc. automatically).
- Cleanup of orphan `session_diff/*.json` files (on exit, toggleable in the GUI).

## Requirements

- Windows (paths and console behavior are Windows-specific).
- [MinGW-w64](https://www.mingw-w64.org/) `g++` (tested with GCC 8.1).
- Vendor libraries are **bundled in this repo**:
  - [Dear ImGui](https://github.com/ocornut/imgui) (`imgui/`)
  - sqlite3 dev lib (`C:\MinGW\opt\lib`, also used by OpenCode itself).

The TUI also requires `ncursesw` (dev libs under `C:\MinGW\opt`).

## Build

```bat
rem TUI (terminal app)
build.bat

rem GUI (Win32 + OpenGL3 + ImGui)
build_gui.bat
```

Build output:
- `opencode-session-manager.exe` — TUI
- `dist\opencode-session-manager-gui.exe` — GUI (**single file, no bundled DLLs**; only links Windows system libraries such as `opengl32.dll`)

## Run

The tool reads the same data as OpenCode:

- Windows data dir: `%USERPROFILE%\.local\share\opencode`

You can override it for testing:

```bat
set OPENCODE_DATA_DIR=C:\some\opencode\data
opencode-session-manager-gui.exe
```

### TUI key bindings

The bottom info line always shows the current row's session id (and the resume command).

| Key | Action |
|-----|--------|
| `up/down` / `j` / `k` | Move cursor |
| `Space` | Toggle selection |
| `Ctrl+A` | Select all |
| `Ctrl+D` | Clear selection |
| `Tab` | Switch view (Sessions / Snapshots) |
| `c` | Copy the resume command `opencode -s <id>` to the clipboard |
| `o` | Resume the cursor session in a new console |
| `Enter` | Delete selected (confirm with `y`) |
| `q` / `Esc` | Quit |

### GUI usage

- Menubar **View** buttons switch between Sessions / Snapshots.
- Per row: **copy** copies the resume command `opencode -s <id>`; **jump** resumes the session in a new terminal.
- Toolbar: **Refresh**, **Select All**, **Clear Sel**, **Delete Selected**, and a **Cleanup orphan diffs on exit** checkbox.

## License

[MIT](LICENSE) © [Linecos](https://github.com/Linecos)

Bundled third-party libs keep their own licenses: Dear ImGui is MIT.