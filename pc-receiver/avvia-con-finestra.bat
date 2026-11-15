@echo off
rem VIIPER e ricevitore con i messaggi in questa finestra (per le prove e i problemi).
rem Ctrl+C o chiusura della finestra fermano tutto. Senza finestra: avvia.bat.
title xbox-eth-gateway - ricevitore PC
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 launcher.py %*
) else (
    python launcher.py %*
)
