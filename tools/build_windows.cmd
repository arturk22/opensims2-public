@echo off
setlocal
rem Configure and build the Windows x64 port. Visual Studio 2022 Build Tools
rem provides cl, cmake and ninja. zlib comes from the workspace vcpkg install.
cd /d "%~dp0.."

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "TOOLCHAIN=%~dp0..\..\..\third_party\vcpkg\scripts\buildsystems\vcpkg.cmake"
set "SDL3=%~dp0..\..\..\third_party\vcpkg\installed\x64-windows\share\sdl3"

if not exist "%VCVARS%" echo missing Visual Studio 2022 Build Tools & exit /b 1
call "%VCVARS%"
if errorlevel 1 exit /b 1

"%CMAKE%" -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" -DSDL3_DIR="%SDL3%"
if errorlevel 1 exit /b 1

"%CMAKE%" --build build-win
if errorlevel 1 exit /b 1

"%CTEST%" --test-dir build-win --output-on-failure -j 8
exit /b %ERRORLEVEL%
