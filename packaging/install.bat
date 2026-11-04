@echo off
chcp 65001 >nul
rem 暖声安装：把文件复制到 Program Files，然后装到默认麦克风上（需要管理员权限）
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set DEST=%ProgramFiles%\WarmVoice
rem 重装时旧文件在用：先关掉暖声窗口和声音服务
taskkill /im WarmVoice.exe /f >nul 2>&1
net stop audiosrv /y >nul 2>&1
echo 正在复制文件到 %DEST% ...
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0WarmVoiceApo.dll" "%DEST%\" >nul || goto :fail
copy /y "%~dp0warmvoice_ai.dll" "%DEST%\" >nul || goto :fail
copy /y "%~dp0wvinstall.exe" "%DEST%\" >nul || goto :fail
copy /y "%~dp0wvproc.exe" "%DEST%\" >nul
copy /y "%~dp0uninstall.bat" "%DEST%\" >nul
copy /y "%~dp0WarmVoice.exe" "%DEST%\" >nul || goto :fail
xcopy /e /i /y /q "%~dp0ui" "%DEST%\ui" >nul || goto :fail
rem 麦克风列表要靠声音服务，马上重新启动它，等 2 秒
net start audiosrv >nul 2>&1
timeout /t 2 /nobreak >nul
rem 开始菜单快捷方式 + 开机自动打开设置窗口（声音处理本身在 Windows 里运行，不需要它）
powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('CommonPrograms')+'\暖声 WarmVoice.lnk');$s.TargetPath='%DEST%\WarmVoice.exe';$s.Save()"
del "%ProgramData%\Microsoft\Windows\Start Menu\Programs\WarmVoice.lnk" >nul 2>&1
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v WarmVoice /t REG_SZ /d "\"%DEST%\WarmVoice.exe\"" /f >nul
echo.
echo 这台电脑上的麦克风：
"%DEST%\wvinstall.exe" list
echo.
echo 正在装到默认麦克风上。会依次试几种方式，每种都用麦克风实际测试一下，
echo 期间电脑声音会断几秒，属于正常。
echo.
"%DEST%\wvinstall.exe" install default
set RC=%errorlevel%
net start audiosrv >nul 2>&1
echo.
echo 正在打开暖声窗口 ...
rem 用 explorer 打开，这样窗口以普通用户身份运行
explorer.exe "%DEST%\WarmVoice.exe"
echo.
pause
exit /b 0
:fail
net start audiosrv >nul 2>&1
echo.
echo 安装失败，请把这个窗口截图发给开发者。
pause
exit /b 1
