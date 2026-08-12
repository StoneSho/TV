@echo off
cd /d "%~dp0"
echo LoveTV PC test page: http://localhost:8000
python -m http.server 8000
