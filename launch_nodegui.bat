@echo off
set PATH=C:\Qt\6.7.3\mingw_64\bin;C:\Qt\Tools\mingw1120_64\bin;%PATH%
set PORT=14608
cd /d "%~dp0"
start "" "build\Source\NodeGUI\NodeGUI.exe" Assets\Examples\foc_demo.json --tcp 127.0.0.1:%PORT% --protocol ivp
