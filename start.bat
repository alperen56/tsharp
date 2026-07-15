@echo off
chcp 65001 >nul
echo building t#...
gcc -O2 main.c lexer.c tparser.c tinterp.c -o tsharp.exe -lcurl -lpthread
if %errorlevel% neq 0 (
    echo build failed. need msys2 mingw64 with: pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-curl
    pause
    exit /b 1
)
echo running...
tsharp.exe %*
pause
