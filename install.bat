@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
set "TARGET_DIR=%LOCALAPPDATA%\NewFolderWithSelection"
set "DLL_PATH=%TARGET_DIR%\NewFolderWithSelection.dll"
set "MANIFEST_PATH=%TARGET_DIR%\AppxManifest.xml"
set "VERB_KEY=HKCU\Software\Classes\AllFileSystemObjects\shell\NewFolderWithSelection"
set "CLSID_KEY=HKCU\Software\Classes\CLSID\{E4B97A12-8C4E-4F39-9A2D-6D81C5F2E101}"
set "UNINSTALL_KEY=HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\NewFolderWithSelection"
set "INTERACTIVE=1"

if /I "%~1"=="--install" (
    set "INTERACTIVE=0"
    goto :DO_INSTALL
)
if /I "%~1"=="--uninstall" (
    set "INTERACTIVE=0"
    goto :DO_UNINSTALL
)

cls
echo ===================================================
echo    New Folder With Selection - Setup
echo ===================================================
echo.
echo   [1] Install / Update
echo   [2] Uninstall
echo   [3] Exit
echo.
choice /C 123 /N /M "Select an option [1-3]: "
if errorlevel 3 exit /b 0
if errorlevel 2 goto :DO_UNINSTALL
if errorlevel 1 goto :DO_INSTALL
exit /b 0

:DO_INSTALL
echo.
echo Installing New Folder With Selection...

set "SRC_DLL=%SCRIPT_DIR%NewFolderWithSelection.dll"
set "SRC_MANIFEST=%SCRIPT_DIR%AppxManifest.xml"
set "SRC_ICON_PNG=%SCRIPT_DIR%icon.png"
set "SRC_ICON_ICO=%SCRIPT_DIR%icon.ico"

if not exist "%SRC_DLL%" (
    set "SRC_DLL=%SCRIPT_DIR%dist\NewFolderWithSelection.dll"
    set "SRC_MANIFEST=%SCRIPT_DIR%src\ShellExtension\AppxManifest.xml"
    set "SRC_ICON_PNG=%SCRIPT_DIR%src\ShellExtension\icon.png"
    set "SRC_ICON_ICO=%SCRIPT_DIR%src\ShellExtension\icon.ico"
)

if not exist "%SRC_DLL%" (
    if exist "%SCRIPT_DIR%build.ps1" (
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build.ps1"
        if errorlevel 1 (
            echo Build failed.
            if "%INTERACTIVE%"=="1" pause
            exit /b 1
        )
    ) else (
        echo Error: NewFolderWithSelection.dll not found. Please extract all files from the zip archive first.
        if "%INTERACTIVE%"=="1" pause
        exit /b 1
    )
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "Get-AppxPackage NewFolderWithSelection -ErrorAction SilentlyContinue | Remove-AppxPackage -ErrorAction SilentlyContinue; " ^
    "Get-Process dllhost -ErrorAction SilentlyContinue | Where-Object { $_.Modules.FileName -match 'NewFolderWithSelection' } | Stop-Process -Force -ErrorAction SilentlyContinue"

reg delete "%UNINSTALL_KEY%" /f >nul 2>&1
if not exist "%TARGET_DIR%" mkdir "%TARGET_DIR%"
del /F /Q "%TEMP%\NewFolderWithSelection_*.dll.old" >nul 2>&1
if exist "%DLL_PATH%" move /Y "%DLL_PATH%" "%TEMP%\NewFolderWithSelection_%RANDOM%.dll.old" >nul 2>&1

copy /Y "%SRC_DLL%" "%DLL_PATH%" >nul
copy /Y "%SRC_MANIFEST%" "%MANIFEST_PATH%" >nul
copy /Y "%SRC_ICON_PNG%" "%TARGET_DIR%\icon.png" >nul
copy /Y "%SRC_ICON_ICO%" "%TARGET_DIR%\icon.ico" >nul
del /F /Q "%TEMP%\NewFolderWithSelection_*.dll.old" >nul 2>&1

regsvr32.exe /s /n /i:user "%DLL_PATH%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Add-AppxPackage -Register '%MANIFEST_PATH%'"

echo Installed successfully to %TARGET_DIR%
if "%INTERACTIVE%"=="1" (
    echo.
    pause
)
exit /b 0

:DO_UNINSTALL
echo.
echo Uninstalling New Folder With Selection...

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "Get-AppxPackage NewFolderWithSelection -ErrorAction SilentlyContinue | Remove-AppxPackage -ErrorAction SilentlyContinue"

if exist "%DLL_PATH%" (
    regsvr32.exe /s /u /n /i:user "%DLL_PATH%"
)

reg delete "%VERB_KEY%" /f >nul 2>&1
reg delete "%CLSID_KEY%" /f >nul 2>&1
reg delete "%UNINSTALL_KEY%" /f >nul 2>&1

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "Get-Process dllhost -ErrorAction SilentlyContinue | Where-Object { $_.Modules.FileName -match 'NewFolderWithSelection' } | Stop-Process -Force -ErrorAction SilentlyContinue"

if exist "%DLL_PATH%" move /Y "%DLL_PATH%" "%TEMP%\NewFolderWithSelection_%RANDOM%.dll.old" >nul 2>&1
if exist "%DLL_PATH%.old" move /Y "%DLL_PATH%.old" "%TEMP%\NewFolderWithSelection_%RANDOM%_2.dll.old" >nul 2>&1
del /F /Q "%TEMP%\NewFolderWithSelection_*.dll.old" >nul 2>&1
if exist "%TARGET_DIR%" rmdir /S /Q "%TARGET_DIR%" >nul 2>&1

echo Uninstalled successfully.
if "%INTERACTIVE%"=="1" (
    echo.
    pause
)
exit /b 0
