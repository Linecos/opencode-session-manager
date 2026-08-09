@echo off
rem Build OpenCode Session Manager GUI (MinGW-w64 + Dear ImGui + Win32 + OpenGL3 + sqlite3)
rem No bundled DLLs - only Windows system libraries (opengl32.dll, dwmapi.dll, ...).
rem Requires g++ from C:\MinGW\bin, dev libs under C:\MinGW\opt, bundled imgui\.
setlocal

set GCC=C:\MinGW\bin\g++
set IMGUI=imgui
set INCS=-I"%IMGUI%" -I"%IMGUI%\backends" -I"C:\MinGW\opt\include"
set LIBS=-L"C:\MinGW\opt\lib" -lsqlite3 -lopengl32 -ldwmapi -luser32 -lgdi32 -lshell32
set OUT=dist

if not exist "%OUT%" mkdir "%OUT%"

%GCC% -std=c++17 -O2 -Wall -static %INCS% ^
    main_gui.cpp opencode_data.cpp ^
    "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" ^
    "%IMGUI%\backends\imgui_impl_win32.cpp" "%IMGUI%\backends\imgui_impl_opengl3.cpp" ^
    %LIBS% -mwindows -o "%OUT%\opencode-session-manager-gui.exe"

if %errorlevel%==0 (
    echo Build OK: %OUT%\opencode-session-manager-gui.exe
) else (
    echo Build FAILED
)
endlocal