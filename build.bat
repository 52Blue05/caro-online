@echo off
REM ==============================================================================
REM Script build tu dong cho Windows: uu tien WSL Ubuntu, fallback MSYS2 MinGW
REM ==============================================================================

wsl -d Ubuntu sh -c "cd /mnt/d/HUST/TH_LTM/prj && make clean && make all" 2>nul
if %ERRORLEVEL% EQU 0 (
    echo.
    echo [+] Build thanh cong tren moi truong chuan WSL Ubuntu!
    exit /b 0
)

echo [!] WSL khong kha dung hoac gap loi, chuyen sang build Windows native voi MSYS2 UCRT64...
set PATH=C:\msys64\ucrt64\bin;%PATH%
make clean
make all
if %ERRORLEVEL% EQU 0 (
    echo.
    echo [+] Build thanh cong tren Windows native (MSYS2 UCRT64)!
) else (
    echo.
    echo [-] Build that bai. Vui long kiem tra lai trinh bien dich GCC hoac WSL.
    exit /b 1
)
