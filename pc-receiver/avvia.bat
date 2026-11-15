@echo off
rem Avvia VIIPER e il ricevitore in background, senza finestra: questa si chiude
rem da sola appena il ricevitore e' in ascolto. Per fermarli: ferma.bat.
rem Con i messaggi a video: avvia-con-finestra.bat.
title xbox-eth-gateway - avvio
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0avvio-automatico.ps1" -Avvia %*
if errorlevel 1 (
    echo.
    pause
) else (
    timeout /t 3 /nobreak >nul 2>nul
)
