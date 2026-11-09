@echo off
setlocal
cd /d "%~dp0"

where xmake >nul 2>nul || (
    echo xmake not found in PATH
    goto :fail
)

xmake f -p windows -a x64 -y || goto :fail
xmake project -k vsxmake -m "debug,releasedbg,release" -a x64 -y || goto :fail

for /d %%D in (vsxmake*) do (
    for %%S in ("%%D\*.sln") do echo Solution: %%~fS
)
call :pause_if_clicked
exit /b 0

:fail
echo Failed to generate Visual Studio solution
call :pause_if_clicked
exit /b 1

:pause_if_clicked
echo %cmdcmdline% | findstr /i /c:"%~nx0" >nul && pause
exit /b 0
