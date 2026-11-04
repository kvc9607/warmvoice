@echo off
rem WarmVoice installer: copies the files to Program Files and puts the effect on your
rem default microphone. Asks for administrator rights (Windows shows a prompt).
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set DEST=%ProgramFiles%\WarmVoice
echo Copying files to %DEST% ...
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0WarmVoiceApo.dll" "%DEST%\" >nul || goto :fail
copy /y "%~dp0warmvoice_ai.dll" "%DEST%\" >nul || goto :fail
copy /y "%~dp0wvinstall.exe" "%DEST%\" >nul || goto :fail
copy /y "%~dp0wvproc.exe" "%DEST%\" >nul
copy /y "%~dp0uninstall.bat" "%DEST%\" >nul
echo.
echo Microphones on this computer:
"%DEST%\wvinstall.exe" list
echo.
echo Installing on the default microphone ...
"%DEST%\wvinstall.exe" install default || goto :fail
echo.
echo Done. Open Settings - System - Sound - your microphone - "Test your microphone",
echo talk for a few seconds, then this window checks that WarmVoice is running:
timeout /t 15
"%DEST%\wvinstall.exe" status default
echo.
pause
exit /b 0
:fail
echo.
echo Installation failed. Please send a screenshot of this window.
pause
exit /b 1
