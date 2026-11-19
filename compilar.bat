@echo off
setlocal enabledelayedexpansion

:: ============================================================================
:: CONFIGURACION DE RUTA DE W64DEVKIT (Si no esta en PATH)
:: Si no te lo encuentra automaticamente, pone entre comillas la ruta a la carpeta
:: bin de tu w64devkit aca abajo. Ejemplo:
:: set "W64DEVKIT_BIN=C:\Users\TuUsuario\Desktop\w64devkit\bin"
:: ============================================================================
set "W64DEVKIT_BIN="

:: 1. Verificar si g++ ya esta en el PATH de Windows
where g++ >nul 2>nul
if %errorlevel% equ 0 goto :COMPILAR

:: 2. Si definiste la ruta manual arriba, verificarla
if defined W64DEVKIT_BIN (
    if exist "%W64DEVKIT_BIN%\g++.exe" (
        set "PATH=%W64DEVKIT_BIN%;%PATH%"
        goto :COMPILAR
    )
)

:: 3. Buscar w64devkit en carpetas comunes relativas
for %%P in (
    "..\w64devkit\bin"
    ".\w64devkit\bin"
    "..\..\w64devkit\bin"
    "C:\w64devkit\bin"
    "D:\w64devkit\bin"
    "%USERPROFILE%\w64devkit\bin"
    "%USERPROFILE%\Desktop\w64devkit\bin"
    "%USERPROFILE%\Downloads\w64devkit\bin"
) do (
    if exist "%%~fP\g++.exe" (
        set "PATH=%%~fP;%PATH%"
        goto :COMPILAR
    )
)

echo =========================================================================
echo [ERROR] No se encontro g++.exe de w64devkit.
echo.
echo SOLUCION:
echo 1. Abri este compilar.bat con el Bloc de notas.
echo 2. En la linea 9 que dice: set "W64DEVKIT_BIN="
echo    pone la ruta a la carpeta bin de tu w64devkit.
echo    Ejemplo: set "W64DEVKIT_BIN=C:\MiCarpeta\w64devkit\bin"
echo =========================================================================
pause
exit /b 1

:COMPILAR
echo =========================================================================
echo Compilando 50 Cent Bulletproof Mouse Mod (xinput1_4.dll)...
echo =========================================================================

g++ -shared -O2 -Wall -Wno-unused-function -o xinput1_4.dll 50_Cent_Bulletproof_Mouse_Mod.cpp mouse_mod_gui.cpp mouse_mod.def -luser32 -lgdi32 -lcomctl32 -lshell32 -static-libgcc -static-libstdc++

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Fallo la compilacion. Revisa los mensajes de arriba.
    pause
    exit /b 1
)

echo.
echo [OK] xinput1_4.dll compilado correctamente!
pause