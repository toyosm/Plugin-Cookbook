@echo off
rem Plugin Studio for Windows: double-click to start. Keep this window open while you work;
rem closing it stops the Studio.
title Plugin Studio
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup\launch.ps1"
if errorlevel 1 pause
