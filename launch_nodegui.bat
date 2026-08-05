@echo off
set PATH=C:\Qt\6.7.3\mingw_64\bin;C:\Qt\Tools\mingw1120_64\bin;%PATH%
cd /d "C:\Users\bc200\.cursor\STMSTUFF"
start "" "build\Source\NodeGUI\NodeGUI.exe" Assets\Examples\foc_demo.json --tcp 127.0.0.1:14608 --protocol ivp
