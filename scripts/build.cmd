@echo off
rem Release build + unit tests. Run from anywhere.
setlocal
set ROOT=%~dp0..
cmake -S "%ROOT%" -B "%ROOT%\build" -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build "%ROOT%\build" --config Release || exit /b 1
"%ROOT%\build\bin\Release\daphne_tests.exe" || exit /b 1
