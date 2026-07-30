@echo off
cd /d "C:\Users\bc200\.cursor\STMSTUFF"
echo Starting HostSim live server...
start "HostSim Live" cmd /k "build\hostsim_svpwm_emitted_build\Debug\host_sim.exe Images\HostSim\scenarios\svpwm_live.json --live"
ping -n 3 127.0.0.1 >nul
echo Starting NodeGUI...
start "NodeGUI" "build\Source\NodeGUI\NodeGUI.exe" Images\NucleoL476FW\svpwm_demo_graph.json --tcp 127.0.0.1:14608 --protocol ivp
