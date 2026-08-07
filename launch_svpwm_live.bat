@echo off
set PORT=14608
cd /d "%~dp0"
echo Starting HostSim live server...
start "HostSim Live" cmd /k "build\hostsim_svpwm_emitted_build\Debug\host_sim.exe Images\HostSim\scenarios\svpwm_live.json --live"
timeout /t 2 /nobreak >nul
echo Starting NodeGUI...
start "NodeGUI" "build\Source\NodeGUI\NodeGUI.exe" Images\NucleoL476FW\svpwm_demo_graph.json --tcp 127.0.0.1:%PORT% --protocol ivp
