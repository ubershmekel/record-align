@echo off
rem Configures and builds with the CMake that ships with Visual Studio 2022.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) do set "CMAKE=%%i"
if not defined CMAKE (
  echo Could not find CMake from Visual Studio. Install the C++ desktop workload.
  exit /b 1
)
"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 || exit /b 1
"%CMAKE%" --build "%~dp0build" --config Release --parallel %* || exit /b 1
echo.
echo Built: %~dp0build\Release\mydaw.exe
