@echo off
rem Ferma VIIPER e il ricevitore in background (avviati da avvia.bat o all'accesso).
rem L'avvio automatico resta attivo: per toglierlo avvio-automatico-disattiva.bat.
title xbox-eth-gateway - arresto
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0avvio-automatico.ps1" -Ferma %*
echo.
pause
