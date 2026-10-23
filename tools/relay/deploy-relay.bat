@echo off
rem Puts the relay for playing with friends far away (worker.js) online, on
rem Cloudflare's free plan. Run it once; run it again only if worker.js changes.
rem You need a free Cloudflare account (dash.cloudflare.com/sign-up). An email
rem made just for this is fine: nothing about the account is public.
setlocal
cd /d "%~dp0"
title DQIX Enhanced - putting the relay online
where node >/dev/null 2>&1
if errorlevel 1 (
  echo Installing Node.js ^(one time, needed for Cloudflare's tool^)...
  winget install --id OpenJS.NodeJS.LTS -e --silent --accept-source-agreements --accept-package-agreements
  set "PATH=%ProgramFiles%\nodejs;%PATH%"
)
where node >/dev/null 2>&1
if errorlevel 1 ( echo Node.js could not be installed. Get it from https://nodejs.org and run this again. & goto fail )
echo.
echo == Signing in to Cloudflare: a browser window opens. Sign in and press Allow.
call npx --yes wrangler@4 login
if errorlevel 1 goto fail
echo.
echo == Putting the relay online
echo If it asks for a workers.dev subdomain: pick a name that says nothing about you
echo ^(it becomes part of the relay's public address^), for example dqix-relay-7k2.
call npx --yes wrangler@4 deploy
if errorlevel 1 goto fail
echo.
echo Done. Copy the https://...workers.dev address shown above and send it to Claude:
echo it goes into the game, so codes work for everyone.
pause
exit /b 0
:fail
echo.
echo Stopped. Send Claude the text above if it isn't clear why.
pause
exit /b 1
