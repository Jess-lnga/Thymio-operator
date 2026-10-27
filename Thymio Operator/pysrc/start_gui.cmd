@echo off
cd /d "%~dp0"
py thymio_operator.py
if errorlevel 1 pause
