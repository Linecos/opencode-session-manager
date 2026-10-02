# OpenCode Session Manager

[![Release](https://img.shields.io/github/v/release/Linecos/opencode-session-manager?sort=semver&color=2ea44f)](https://github.com/Linecos/opencode-session-manager/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-0078D6.svg)](#requirements)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](#requirements)
[![Dear ImGui 1.92](https://img.shields.io/badge/Dear%20ImGui-1.92-9b59b6.svg)](https://github.com/ocornut/imgui)
[![Tests](https://img.shields.io/badge/tests-96%20passing-2ea44f)](tests/test_data.cpp)

**English** · [简体中文](README.zh-CN.md)

A desktop tool to visually browse and clean old [OpenCode](https://opencode.ai) conversations, orphan
snapshot directories and stale `session_diff` files — as a **terminal (TUI)** and a **GUI**
application, written in C++17 for Windows.

## Features

- Browse all OpenCode sessions, snapshot directories and their sizes.
- **Both front ends share the same feature set** and the same filter/sort code
  (`session_view.cpp`), so they cannot drift apart.
- **Sessions** view: id, title, project / worktree, message count, created & updated.
- **Snapshots** view: marked `in use` (a session still references that project) or `no session`
  (orphan), with directory size.
- Select multiple items and remove them, with a confirmation dialog that lists exactly what goes.
- **Nothing is deleted outright**: files are sent to the Windows **Recycle Bin** by default.
- Live search across id / title / project / path, sortable columns, and an "orphans only" filter.
- Per row: **copy** the resume command `opencode -s <id>`, **jump** to resume the session in a new
  console (in its worktree when it still exists), **dir** / **open** to reveal the folder in Explorer.
- Snapshot sizes are measured a few directories at a time, so neither front end stalls on a large
  data directory.
- Chinese/CJK font support (loads Microsoft YaHei etc. automatically), DPI-aware on scaled displays
  and when dragging between monitors.
- Orphan `session_diff/*.json` cleanup, either on demand or (opt-in) on exit.
- A headless `--list` mode, so the same pipeline can be used from scripts.

## Safety

- Removals use `SHFileOperation(FOF_ALLOWUNDO)`, i.e. the **Recycle Bin**, so a wrong click is
  recoverable. Set `OPENCODE_SM_NO_RECYCLE=1` (or pass `--no-recycle` to the TUI) to delete
  permanently instead.
- The "cleanup on exit" behaviour is **off by default** in both front ends; use the checkbox, the
  `Clean orphan diffs` button, or `--cleanup`.
- Deleting a session also removes the rows that belong to it (`message`, `part`, `session_message`,
  `session_input`, `todo`, ...). opencode's schema declares foreign keys for only a couple of these
  tables, so the tool also treats "any table with a `session_id` column" as session-scoped.
- The delete runs in a single transaction with a 5 second busy timeout, and reports failures instead
  of pretending it worked (for example when opencode is holding the database).

## Requirements

- Windows (paths and console behaviour are Windows-specific).
- [MinGW-w64](https://www.mingw-w64.org/) `g++` with C++17 (tested with GCC 8.1).
- sqlite3 dev lib (`C:\MinGW\opt` in the defaults below).
- The TUI also needs `ncursesw`.
- [Dear ImGui](https://github.com/ocornut/imgui) is **bundled in this repo** (`imgui/`).

## Build

```bat
rem TUI (terminal app)  -> dist\opencode-session-manager.exe
build.bat

rem GUI (Win32 + OpenGL3 + ImGui) -> dist\opencode-session-manager-gui.exe
build_gui.bat

rem data layer tests    -> dist\test_data.exe (+ runs them)
build_tests.bat
```

The GUI is a **single file, no bundled DLLs** (only Windows system libraries such as
`opengl32.dll`).

If your toolchain lives somewhere else, override it — the scripts are no longer hard-wired to
`C:\MinGW`:

```bat
set MINGW_ROOT=D:\msys64\ucrt64
set MINGW_OPT=D:\msys64\ucrt64\opt
build_gui.bat
```

With `mingw32-make` installed, the `Makefile` does the same:

```bat
mingw32-make            rem TUI + GUI + tests
mingw32-make gui
mingw32-make test       rem builds and runs the tests
mingw32-make clean
```

The version compiled into the executables comes from a single file, `version.h`, which `resource.rc`
and the C++ sources both include.

## Tests

`build_tests.bat` (or `mingw32-make test`) builds and runs `tests/test_data.cpp`, which covers the
things that are easy to get wrong and impossible to notice by clicking around:

- non-ASCII paths (a Chinese `%USERPROFILE%`, worktree and snapshot directory),
- the delete cascade against a schema shaped like the real one (child tables that carry a
  `session_id` without declaring a foreign key),
- a locked database (the delete must fail loudly, not silently),
- the shared view model: filtering (including CJK and ASCII case folding), every sort field and
  direction, tie-break determinism, and that building a view never mutates its input,
- orphan diff cleanup, UTF-8 safe truncation, and the resume-command argument guard.

```
96 checks, 0 failed
```

A helper mode is available for inspecting a real database:

```bat
dist\test_data.exe --delete <path\to\opencode.db> <session-id> [<session-id> ...]
```

## Run

The tool reads the same data as OpenCode:

- Windows data dir: `%USERPROFILE%\.local\share\opencode`

Override it for testing:

```bat
set OPENCODE_DATA_DIR=C:\some\opencode\data
dist\opencode-session-manager-gui.exe
```

Other environment variables:

| Variable | Effect |
|----------|--------|
| `OPENCODE_DATA_DIR` | Data directory to read (`opencode.db`, `storage/`, `snapshot/`) |
| `OPENCODE_SM_NO_RECYCLE` | `1` = delete permanently instead of using the Recycle Bin |

Settings (currently just the text size) are stored per user in
`%APPDATA%\opencode-session-manager\settings.ini`.

### TUI key bindings

| Key | Action |
|-----|--------|
| `up/down` / `j` / `k` | Move cursor |
| `PgUp` / `PgDn`, `Home` / `End` | Page up/down, first/last row |
| `Space` | Toggle selection |
| `Ctrl+A` | Select all visible rows |
| `Ctrl+D` | Clear selection |
| `Tab` | Switch view (Sessions / Snapshots) |
| `F5` | Reload data |
| `/` | Filter (live, `Enter` applies, `Esc` cancels) |
| `s` / `S` | Cycle the sort field / reverse the direction |
| `d` or `Enter` | Remove selected (confirm with `y`, cancel with `n`) |
| `c` | Copy the resume command `opencode -s <id>` |
| `o` | Resume the cursor session in a new console |
| `r` | Reveal the worktree / snapshot folder in Explorer |
| `O` | Snapshots view: orphan directories only |
| `x` | Clean orphan `session_diff` files now |
| `Ctrl+E` | Toggle cleanup-on-exit |
| `Esc` | Clear the filter, or quit when there is none |
| `q` | Quit |

The header line shows the filtered/total row count, the active sort and how much space the orphan
snapshot directories hold. Snapshot sizes are measured a few directories at a time, so the UI stays
responsive on large data directories (it shows `sizing...` meanwhile, and `...` per row).

### Headless listing

The same load/filter/sort pipeline is available without the UI, which makes the tool scriptable:

```bat
rem the 10 biggest orphan snapshot directories
opencode-session-manager.exe --list --snapshots --orphans --limit 10

rem sessions mentioning a project, sorted by message count
opencode-session-manager.exe --list --filter nixStats --sort msgs
```

| Option | Effect |
|--------|--------|
| `--list` | print the current view and exit |
| `--snapshots` | list snapshot directories instead of sessions |
| `--orphans` | `--snapshots`: only directories no session references |
| `--filter <text>` | match id / title / project / worktree (or path / name) |
| `--sort <field>` | `created`, `updated`, `msgs`, `size`, `status`, `id`, `name`, `project` |
| `--asc` | sort ascending (default: descending) |
| `--limit <n>` | print at most n rows |
| `--cleanup` | also clean orphan `session_diff` files after the UI exits |
| `--no-recycle` | delete permanently instead of using the Recycle Bin |
| `--version` | print the version and exit |

### GUI usage

- **View** buttons switch between Sessions / Snapshots; the menu bar shows the totals and how much
  space the orphan snapshot directories hold.
- Toolbar: **Refresh**, **Select All**, **Clear Sel**, **Delete Selected**, **Clean orphan diffs**,
  a **Cleanup on exit** checkbox, a search box, an **Orphans only** filter and the text size control.
- Click a column header to sort (by default: sessions by creation date, snapshots by size — largest
  first, which is the usual reason to open this tool).
- Keyboard: `F5` refresh, `Del` remove selected, `Ctrl+A` select all, `Ctrl+D` clear selection,
  `Ctrl+F` focus the search box, `Ctrl++` / `Ctrl+-` / `Ctrl+0` text size, `Esc` clear the
  search / selection.
- Per row actions: **copy** (resume command), **jump** (resume in a new console), **dir** /
  **open** (reveal in Explorer).
- **Text size** is adjustable from the toolbar (`Text: 24 px  -  +  reset`); the setting is saved to
  `%APPDATA%\opencode-session-manager\settings.ini`. The default reproduces the size earlier builds
  rendered at — see the note below.

### A note on text size and DPI

Dear ImGui 1.92 computes `GetFontSize()` as
`style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi`. Earlier builds passed an
already-DPI-scaled pixel size to the font atlas *and* set `FontScaleDpi`, so text always rendered at
`scale²` — 36px on a 150% display instead of 24px. That is fixed (the monitor scale is applied once),
which on its own makes the text look smaller than people were used to, so the text size became a
setting whose default matches the old appearance. `Ctrl+0` / **reset** returns to that default;
keep pressing `Ctrl+-` to reach the size a plain 16px UI would use at 100%.

Column widths are derived from the live font metrics, so the action buttons cannot be clipped at any
text size.

## License

[MIT](LICENSE) © [Linecos](https://github.com/Linecos)

Bundled third-party libs keep their own licenses: Dear ImGui is MIT. sqlite3 is public domain.
