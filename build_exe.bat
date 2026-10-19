@echo off
chcp 65001 >nul
echo ============================================
echo   Fabrication de coop.exe (une seule fois)
echo ============================================
echo.
cd /d "%~dp0"
py -m pip install --upgrade pyinstaller numpy psutil
if errorlevel 1 goto erreur
set EXTRA=
if exist icon.ico set EXTRA=--icon icon.ico --add-data "icon.ico;."
if exist icon.ico echo Icone trouvee : icon.ico
if exist logo.png set EXTRA=%EXTRA% --add-data "logo.png;."
if exist mec_overlay.dll set EXTRA=%EXTRA% --add-binary "mec_overlay.dll;."
if exist mec_overlay.dll echo Overlay trouve : mec_overlay.dll
py -m PyInstaller --onefile --noconsole --name coop --clean %EXTRA% coop.py
if errorlevel 1 goto erreur
copy /y dist\coop.exe coop.exe >nul
rmdir /s /q build >nul 2>&1
rmdir /s /q dist >nul 2>&1
del /q coop.spec >nul 2>&1
echo.
echo Fini ! coop.exe est dans ce dossier. Tu peux l'envoyer a tes potes.
echo (Ils n'ont plus besoin d'installer Python.)
pause
exit /b 0

:erreur
echo.
echo Il y a eu une erreur. Fais une capture de cette fenetre pour Claude.
pause
exit /b 1
