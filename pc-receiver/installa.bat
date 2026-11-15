@echo off
rem Prepara il PC una volta sola: Python, usbip-win2, VIIPER, firewall.
rem Dopo si usa avvia.bat. Con "installa.bat -CheckOnly" controlla soltanto.
title xbox-eth-gateway - preparazione del PC
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installa.ps1" %*
echo.
pause
