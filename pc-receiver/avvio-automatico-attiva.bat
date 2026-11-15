@echo off
rem Attiva l'avvio automatico del ricevitore all'accesso a Windows.
rem Vedi avvio-automatico.ps1 per le opzioni.
title xbox-eth-gateway - avvio automatico
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0avvio-automatico.ps1" -Attiva %*
echo.
pause
