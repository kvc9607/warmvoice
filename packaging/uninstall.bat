@echo off
rem Removes WarmVoice from all microphones and restores the original settings.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set DEST=%ProgramFiles%\WarmVoice
"%DEST%\wvinstall.exe" uninstall all
echo The files stay in %DEST% until the next restart; you can delete that folder afterwards.
pause
