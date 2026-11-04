@echo off
rem WarmVoice installer: copies the files to Program Files and puts the effect on your
rem default microphone. Asks for administrator rights (Windows shows a prompt).
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set DEST=%ProgramFiles%\WarmVoice
rem On a re-install the old files are in use: close the app and let go of the APO first.
taskkill /im WarmVoice.exe /f >nul 2>&1
net stop audiosrv /y >nul 2>&1
echo Copying files to %DEST% ...
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0WarmVoiceApo.dll" "%DEST%\" >nul || goto :fail
copy /y "%~dp0warmvoice_ai.dll" "%DEST%\" >nul || goto :fail
copy /y "%~dp0wvinstall.exe" "%DEST%\" >nul || goto :fail
copy /y "%~dp0wvproc.exe" "%DEST%\" >nul
copy /y "%~dp0uninstall.bat" "%DEST%\" >nul
copy /y "%~dp0WarmVoice.exe" "%DEST%\" >nul || goto :fail
xcopy /e /i /y /q "%~dp0ui" "%DEST%\ui" >nul || goto :fail
rem Start menu shortcut + start with Windows (for the settings panel; the sound processing
rem itself runs inside Windows and needs neither)
powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('CommonPrograms')+'\WarmVoice.lnk');$s.TargetPath='%DEST%\WarmVoice.exe';$s.Save()"
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v WarmVoice /t REG_SZ /d "\"%DEST%\WarmVoice.exe\"" /f >nul
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
echo Opening the WarmVoice window ...
rem explorer starts it as the normal (not administrator) user
explorer.exe "%DEST%\WarmVoice.exe"
pause
exit /b 0
:fail
echo.
echo Installation failed. Please send a screenshot of this window.
pause
exit /b 1
