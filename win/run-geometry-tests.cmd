@echo off
rem Geometry tests for resize_geometry.h. vcvars64 breaks on the Cyrillic
rem user profile, so MSVC/SDK paths are set explicitly.
set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "CL_EXE=%MSVC%\bin\Hostx64\x64\cl.exe"
set "INCLUDE=%MSVC%\include;%SDK%\Include\10.0.26100.0\um;%SDK%\Include\10.0.26100.0\shared;%SDK%\Include\10.0.26100.0\ucrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\10.0.26100.0\um\x64;%SDK%\Lib\10.0.26100.0\ucrt\x64"
set "RUNNER=%~dp0..\app\windows\runner"
if not exist "%TEMP%\kload-geom" mkdir "%TEMP%\kload-geom"
"%CL_EXE%" /nologo /std:c++17 /EHsc /W3 /WX /utf-8 /Fo"%TEMP%\kload-geom\geom.obj" /Fe"%TEMP%\kload-geom\geometry_tests.exe" "%RUNNER%\resize_geometry_test.cpp" >nul
if errorlevel 1 (
  echo test compilation failed
  exit /b 1
)
"%TEMP%\kload-geom\geometry_tests.exe"
if errorlevel 1 exit /b 1

"%CL_EXE%" /nologo /std:c++17 /EHsc /W3 /utf-8 /Fo"%TEMP%\kload-geom\d.obj" /Fe"%TEMP%\kload-geom\drag_tests.exe" "%RUNNER%\drag_payload_test.cpp" >nul
if errorlevel 1 (
  echo drag payload test compilation failed
  exit /b 1
)
"%TEMP%\kload-geom\drag_tests.exe"
exit /b %errorlevel%
