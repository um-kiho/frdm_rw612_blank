@echo off
REM Build script for MCUXpresso IDE on Windows
REM Called from project dir (frdm_rw612_blank\); west must run from workspace root.
REM Usage: build.bat [build|clean|configure|flash] [debug|release]

setlocal

REM Move up to west workspace root (parent of this script's directory)
cd /d "%~dp0.."

set BOARD=frdm_rw612
set SRC_DIR=frdm_rw612_blank
set ZEPHYR_TOOLCHAIN_VARIANT=zephyr
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\User\zephyr-sdk-1.0.1

REM BT Classic (A2DP) requires mbedtls, tf-psa-crypto, and libsbc modules.
REM Download them on first use - runs only once, skipped on subsequent builds.
if not exist "modules\crypto\mbedtls" goto DO_UPDATE
if not exist "modules\crypto\tf-psa-crypto" goto DO_UPDATE
goto SKIP_UPDATE
:DO_UPDATE
echo [build.bat] Required modules missing. Running: west update
west update
if errorlevel 1 (
    echo ERROR: west update failed. Check network and try again.
    goto END
)
echo [build.bat] Modules downloaded successfully.
:SKIP_UPDATE


REM Determine build directory from second argument (default: debug)
if /i "%2"=="release" (
    set BUILD_DIR=frdm_rw612_blank/release
    set EXTRA_CMAKE=-DCONFIG_SIZE_OPTIMIZATIONS=y -DCONFIG_LOG_DEFAULT_LEVEL=0
) else (
    set BUILD_DIR=frdm_rw612_blank/debug
    set EXTRA_CMAKE=
)

REM Check command line argument
if "%1"=="" goto BUILD
if /i "%1"=="build"     goto BUILD
if /i "%1"=="all"       goto BUILD
if /i "%1"=="clean"     goto CLEAN
if /i "%1"=="configure" goto CONFIGURE
if /i "%1"=="flash"     goto FLASH

echo Unknown target: %1
goto END

:CONFIGURE
echo Configuring [%BUILD_DIR%]...
if "%EXTRA_CMAKE%"=="" (
    west build -b %BOARD% -d %BUILD_DIR% --cmake-only %SRC_DIR%
) else (
    west build -b %BOARD% -d %BUILD_DIR% --cmake-only %SRC_DIR% -- %EXTRA_CMAKE%
)
goto END

:BUILD
echo Building [%BUILD_DIR%]...
if "%EXTRA_CMAKE%"=="" (
    west build -b %BOARD% -d %BUILD_DIR% %SRC_DIR%
) else (
    west build -b %BOARD% -d %BUILD_DIR% %SRC_DIR% -- %EXTRA_CMAKE%
)
goto END

:CLEAN
echo Cleaning [%BUILD_DIR%]...
if exist "%BUILD_DIR%" (
    rmdir /s /q "%BUILD_DIR%"
)
echo Clean complete.
goto END

:FLASH
call :BUILD
echo Flashing to %BOARD%...
west flash -d %BUILD_DIR%
goto END

:END
endlocal
