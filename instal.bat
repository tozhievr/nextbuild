@echo off
setlocal
set "NEXTBUILD_INSTALLER=%~dp0install.ps1"
if exist "%~dp0VERSION" if exist "%NEXTBUILD_INSTALLER%" goto run
set "NEXTBUILD_TEMP_INSTALLER=%TEMP%\nextbuild-install-%RANDOM%-%RANDOM%.ps1"
set "NEXTBUILD_INSTALLER=%NEXTBUILD_TEMP_INSTALLER%"
curl.exe -fsSL --proto "=https" --proto-redir "=https" -o "%NEXTBUILD_INSTALLER%" "https://raw.githubusercontent.com/tozhievr/nextbuild/main/install.ps1"
if errorlevel 1 goto cleanup

:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%NEXTBUILD_INSTALLER%" %*

:cleanup
set "NEXTBUILD_INSTALL_STATUS=%ERRORLEVEL%"
if defined NEXTBUILD_TEMP_INSTALLER if exist "%NEXTBUILD_TEMP_INSTALLER%" del /q "%NEXTBUILD_TEMP_INSTALLER%"
exit /b %NEXTBUILD_INSTALL_STATUS%
