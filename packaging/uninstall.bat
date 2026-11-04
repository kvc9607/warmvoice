@echo off
rem Removes WarmVoice from all microphones and restores the original settings.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set DEST=%ProgramFiles%\WarmVoice
taskkill /im WarmVoice.exe /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v WarmVoice /f >nul 2>&1
del "%ProgramData%\Microsoft\Windows\Start Menu\Programs\WarmVoice.lnk" >nul 2>&1
"%DEST%\wvinstall.exe" uninstall all
echo The files stay in %DEST% until the next restart; you can delete that folder afterwards.
pause
