@echo off
cd /d "%~dp0"
if not exist node_modules call npm install
if not exist .env (
  copy .env.example .env >nul
  echo Rellena .env con tu token, GUILD_ID y VOICE_CHANNEL_ID y vuelve a ejecutar.
  notepad .env
  exit /b 1
)
node --env-file=.env src/index.js
pause
