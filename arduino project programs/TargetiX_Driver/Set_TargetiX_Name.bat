@echo off
chcp 65001 > nul
title Реєстрація назви TargetiX

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ПОМИЛКА] Запустіть файл від імені Адміністратора!
    pause
    exit /b
)

echo Встановлення назви TargetiX для всіх виявлених адаптерів CP2102...

powershell -Command "$devices = Get-PnpDevice | Where-Object { $_.HardwareID -like '*VID_10C4&PID_EA60*' }; foreach ($d in $devices) { Set-ItemProperty -Path ('HKLM:\SYSTEM\CurrentControlSet\Enum\' + $d.InstanceId) -Name 'FriendlyName' -Value 'TargetiX UKRAINE' -ErrorAction SilentlyContinue }"

echo.
echo ========================================================
echo [УСПІХ] Назву змінено на TargetiX!
echo Відключіть та знову підключіть пристрій до USB.
echo ========================================================
pause