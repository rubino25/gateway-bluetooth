@echo off
rem Ferma il ricevitore in background e toglie l'avvio automatico.
rem Vedi avvio-automatico.ps1 per le opzioni.
title xbox-eth-gateway - avvio automatico
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0avvio-automatico.ps1" -Disattiva %*
echo.
pause
