@echo off
cd /d "%~dp0"
git pull || exit /b 1
cmake -S project -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug || exit /b 1
cmake --build build || exit /b 1
build\hello.exe