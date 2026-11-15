@echo off
setlocal

set VS=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe

if not exist "%CMAKE%" (
    echo cmake not found at %CMAKE%
    echo edit VS= in this script if Visual Studio lives somewhere else
    exit /b 1
)

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "Visual Studio 18 2026" -A x64
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%~dp0build" --config %CONFIG% -- /m
if errorlevel 1 exit /b 1

echo.
echo built -^> %~dp0build\bin\PdfEditor.exe
