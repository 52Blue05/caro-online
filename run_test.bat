@echo off
setlocal DisableDelayedExpansion
REM ==============================================================================
REM Script chay toan bo bo test tu dong G2 tren Windows thong qua WSL Ubuntu
REM ==============================================================================

echo [*] Dang chay bo kiem thu tu dong Moc G2 tren WSL Ubuntu...
wsl -d Ubuntu sh -c "cd /mnt/d/HUST/TH_LTM/prj && bash tests/test_g2.sh"
set TEST_STATUS=%ERRORLEVEL%

if %TEST_STATUS% EQU 0 (
    echo.
    echo [+] TAT CA CAC BAI TEST MOC G2 DA VUOT QUA (PASS 100%%^)!
    exit /b 0
) else (
    echo.
    echo [-] Co bai test khong vuot qua. Vui long kiem tra log tai tests/test_g2_results.log
    exit /b %TEST_STATUS%
)
