@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64
cmake --build "E:\PinyonShift\work\.local\rexglue\out\build\win-amd64" --config Release