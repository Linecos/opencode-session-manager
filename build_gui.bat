@echo off
rem Build OpenCode Session Manager GUI (MinGW-w64 + Dear ImGui + SDL3 + OpenGL3 + sqlite3)
rem Requires g++ from C:\MinGW\bin, dev libs under C:\MinGW\opt, bundled imgui\ and third_party\SDL3\.
setlocal

set GCC=C:\MinGW\bin\g++
set IMGUI=imgui
set SDL=third_party\SDL3
set INCS=-I"%IMGUI%" -I"%IMGUI%\backends" -I"%SDL%\include" -I"C:\MinGW\opt\include"
set LIBS=-L"%SDL%\lib" -lSDL3 -lopengl32 -L"C:\MinGW\opt\lib" -lsqlite3
set OUT=dist

if not exist "%OUT%" mkdir "%OUT%"

%GCC% -std=c++17 -O2 -Wall -DNCURSES_WIDECHAR %INCS% ^
    main_gui.cpp opencode_data.cpp ^
    "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" ^
    "%IMGUI%\backends\imgui_impl_sdl3.cpp" "%IMGUI%\backends\imgui_impl_opengl3.cpp" ^
    %LIBS% -mwindows -o "%OUT%\opencode-session-manager-gui.exe"

if %errorlevel%==0 (
    copy /Y "%SDL%\bin\SDL3.dll" "%OUT%\SDL3.dll" >nul
    echo Build OK: %OUT%\opencode-session-manager-gui.exe
) else (
    echo Build FAILED
)
endlocal