@echo off
rem Dice se l'avvio automatico e' attivo e se il ricevitore sta girando.
rem Vedi avvio-automatico.ps1 per le opzioni.
title xbox-eth-gateway - avvio automatico
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0avvio-automatico.ps1" -Stato %*
echo.
pause
