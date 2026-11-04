@echo off
chcp 65001 >nul
rem 卸载暖声：从所有麦克风上去掉，恢复原来的设置
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set DEST=%ProgramFiles%\WarmVoice
taskkill /im WarmVoice.exe /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v WarmVoice /f >nul 2>&1
del "%ProgramData%\Microsoft\Windows\Start Menu\Programs\WarmVoice.lnk" >nul 2>&1
del "%ProgramData%\Microsoft\Windows\Start Menu\Programs\暖声 WarmVoice.lnk" >nul 2>&1
"%DEST%\wvinstall.exe" uninstall all
echo.
echo 已卸载。%DEST% 文件夹重启电脑后可以手动删掉。
pause
