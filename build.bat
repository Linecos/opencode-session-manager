@echo off
rem Build OpenCode Session Manager (MinGW-w64 + ncursesw + sqlite3)
rem Requires g++ from C:\MinGW\bin and dev libs under C:\MinGW\opt
setlocal

set GCC=C:\MinGW\bin\g++
set INCS=-I"C:\MinGW\opt\include"
set LIBS=-L"C:\MinGW\opt\lib" -lncursesw -lsqlite3
set OUT=dist

if not exist "%OUT%" mkdir "%OUT%"

%GCC% -std=c++17 -O2 -Wall -static -DNCURSES_WIDECHAR %INCS% main.cpp opencode_data.cpp %LIBS% -o "%OUT%\opencode-session-manager.exe"
if %errorlevel%==0 (
    echo Build OK: %OUT%\opencode-session-manager.exe
) else (
    echo Build FAILED
)
endlocal