@echo off
set PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin;%PATH%
cd /d %~dp0
solver2048.exe --serve --port 8766 --ms 5000 --threads 4 --depth 13 > server.log 2> server.err
