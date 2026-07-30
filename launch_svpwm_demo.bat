@echo off
REM =================================================================
REM  SVPWM Live Demo Launcher
REM  Starts HostSim (live mode) + NodeGUI for real-time simulation
REM =================================================================

cd /d "C:\Users\bc200\.cursor\STMSTUFF"

echo.
echo ===== Starting HostSim (live mode, port 14608) =====
start "HostSim" cmd /c "build\hostsim_emitted_build\Debug\host_sim.exe Images\HostSim\scenarios\svpwm_live.json --live"

echo Waiting for HostSim to start listening...
timeout /t 2 /nobreak >nul

echo.
echo ===== Starting NodeGUI (SVPWM graph) =====
start "NodeGUI" "build\Source\NodeGUI\NodeGUI.exe" Images\NucleoL476FW\svpwm_demo_graph.json --tcp 127.0.0.1:14608 --protocol ivp

echo.
echo ===== Both running! =====
echo.
echo NodeGUI Console Commands:
echo   throttle a 0.5      Set modulation index (0.0 - 1.15)
echo   throttle b 0.3      Set electrical freq map input (0.0 - 1.0)
echo   pause / resume      Pause/resume simulation
echo   quit                Stop everything
echo.
