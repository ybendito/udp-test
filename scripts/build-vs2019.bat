@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\Tools\VsDevCmd.bat" -arch=amd64
cd /d "%~dp0..\build"
cmake ..
if errorlevel 1 exit /b 1
cmake --build . --config Release
exit /b %errorlevel%
