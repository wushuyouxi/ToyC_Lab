@echo off
rem Differential test helper: ToyC source is a valid C subset, compile it with MSVC as reference.
rem Usage: compile_c.bat input.tc output.exe
rem NOTE: /utf-8 is REQUIRED - sources contain UTF-8 Chinese comments; MSVC's default
rem local-codepage (GBK) decoding silently corrupts parsing.
rem NOTE: keep THIS file ASCII-only (cmd decodes bat files in the local codepage).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul 2>&1
cl /nologo /EHsc /O2 /utf-8 /TC "%~1" /Fe:"%~2" >nul 2>&1
