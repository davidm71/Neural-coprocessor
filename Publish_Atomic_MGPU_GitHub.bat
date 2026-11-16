@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Atomic MGPU - publish selected source files to the public fps-hud branch.
rem Usage: Publish_Atomic_MGPU_GitHub.bat [optional commit message]
rem This script intentionally does NOT stage dist\, assets\production\, or build outputs.

set "REPO=C:\MGPU\Neural-coprocessor-main"
set "BRANCH=fps-hud"
set "REMOTE=origin"
set "COMMIT_EMAIL=1226989+davidm71@users.noreply.github.com"
set "MESSAGE=Update Atomic MGPU source and documentation"
if not "%~1"=="" set "MESSAGE=%*"

where git >nul 2>&1
if errorlevel 1 (
  echo ERROR: Git is not installed or cannot be found in PATH.
  goto :fail
)
if not exist "%REPO%\.git" (
  echo ERROR: Repository not found at "%REPO%".
  goto :fail
)
cd /d "%REPO%" || goto :fail

for /f "delims=" %%B in ('git branch --show-current') do set "CURRENT_BRANCH=%%B"
if /I not "%CURRENT_BRANCH%"=="%BRANCH%" (
  echo ERROR: Current branch is "%CURRENT_BRANCH%", expected "%BRANCH%".
  echo Switch branches manually and try again.
  goto :fail
)
git remote get-url "%REMOTE%" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Git remote "%REMOTE%" is unavailable.
  goto :fail
)

echo.
echo ===== ATOMIC MGPU GITHUB PUBLISH =====
echo Repository: %REPO%
echo Branch: %BRANCH%
echo Remote: %REMOTE%
echo.
echo WARNING: This branch is PUBLIC. Do not publish paid/private code,
echo credentials, proprietary DLLs, or licensed binaries without permission.
echo.
echo Current status:
git status --short
if errorlevel 1 goto :fail

rem Require an empty staging area so we don't accidentally commit unrelated staged work.
git diff --cached --quiet
if errorlevel 2 (
  echo ERROR: Unable to inspect staged changes.
  goto :fail
)
if errorlevel 1 (
  echo ERROR: Some changes are already staged. Review them manually first:
  git --no-pager diff --cached --name-only
  goto :fail
)

rem Explicit allowlist: tracked and new source files in these directories.
rem Skip missing paths (Git rejects unmatched pathspecs).
for %%P in ("CMakeLists.txt" "README.md" "THIRD_PARTY.md" "FORK_NOTES.md" "COMMUNITY_20FPS_README.md" "COMMUNITY_30FPS_README.md" "src" "docs" "tests" "installer" "tools" "assets\mgpu.ini" ".github" "Publish_Atomic_MGPU_GitHub.bat") do (
  if exist "%%~P" (
    git add -A -- "%%~P"
    if errorlevel 1 (
      echo ERROR: Failed to stage %%~P.
      goto :fail
    )
  )
)

git diff --cached --quiet
if errorlevel 2 (
  echo ERROR: Unable to inspect staged changes.
  goto :fail
)
if not errorlevel 1 (
  echo.
  echo No changes in the approved source/documentation paths to commit.
  echo Nothing was pushed.
  goto :success
)

git diff --cached --check
if errorlevel 1 (
  echo ERROR: Whitespace problems detected. Review staged diff.
  goto :fail
)

echo.
echo ===== FILES TO BE COMMITTED =====
git --no-pager diff --cached --stat
echo.
echo ===== FILE NAMES =====
git --no-pager diff --cached --name-only
echo.
choice /C YN /M "Commit these files and push to public GitHub"
if errorlevel 2 (
  echo Cancelled. Files remain staged for review.
  goto :fail
)

rem Use GitHub noreply email for this commit to avoid GH007 privacy rejection.
git -c "user.email=%COMMIT_EMAIL%" commit -m "%MESSAGE%"
if errorlevel 1 (
  echo ERROR: Commit failed. Resolve the issue before retrying.
  goto :fail
)

echo.
echo ===== SYNCHRONIZING REMOTE =====
git fetch "%REMOTE%" "%BRANCH%"
if errorlevel 1 (
  echo ERROR: Fetch failed. Commit is saved locally; retry push later.
  goto :fail
)
git rebase "%REMOTE%/%BRANCH%"
if errorlevel 1 (
  echo ERROR: Rebase failed or conflicted. Resolve manually, then push.
  echo Do NOT run this script again while a rebase is in progress.
  goto :fail
)

echo.
echo ===== PUSHING TO GITHUB =====
git push "%REMOTE%" "%BRANCH%"
if errorlevel 1 (
  echo ERROR: Push failed. Your commit remains local.
  goto :fail
)

echo.
echo Published successfully:
git --no-pager log -1 --format="%%h %%s"
git status --short
goto :success

:fail
echo.
echo Publish did not finish. Review the message above.
exit /b 1

:success
echo.
echo Finished.
exit /b 0
