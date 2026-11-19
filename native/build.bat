@echo off
REM Requiere: Visual Studio (Build Tools con C++), CMake y JAVA_HOME apuntando a un JDK 21
cmake -S . -B build -A x64
cmake --build build --config Release
