@echo off
setlocal EnableDelayedExpansion

REM Garante execucao na pasta onde o script esta localizado (evita C:\Windows\System32)
cd /d "%~dp0"

echo ============================================================
echo   FABPRO-Q4
echo   Build Windows 64-bit (Visual Studio + JUCE 8.0.4)
echo ============================================================

set "JUCE_PATH="
if not "%JUCE_DIR%"=="" (
    set "JUCE_PATH=-DJUCE_DIR=%JUCE_DIR%"
)

cmake -B build -G "Visual Studio 17 2022" -A x64 %JUCE_PATH%
if errorlevel 1 (
    echo [AVISO] Visual Studio 2022 nao detectado, tentando gerador padrao x64...
    cmake -B build -A x64 %JUCE_PATH%
    if errorlevel 1 (
        echo [ERRO] Falha na configuracao do CMake. Verifique se o JUCE esta em C:\JUCE, .\JUCE ou defina JUCE_DIR.
        exit /b 1
    )
)

cmake --build build --config Release --parallel
if errorlevel 1 (
    echo [ERRO] Falha na compilacao Release.
    exit /b 1
)

echo.
echo [SUCESSO] Compilacao concluida!
echo Artefatos gerados em:
echo   VST3:       build\ProQ3Clone_artefacts\Release\VST3\FABPRO-Q4.vst3
echo   Standalone: build\ProQ3Clone_artefacts\Release\Standalone\FABPRO-Q4.exe
echo ============================================================
pause
