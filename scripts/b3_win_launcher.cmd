@echo off
rem Bayonetta 3 recomp: the launcher window on Windows (double-click this file).
rem Settings, the controller check and Play; Play runs `tools\b3_win.py play`, which needs a finished
rem `python tools\b3_win.py setup --dump <your dump folder>` (docs\PLAYING.md, "Windows").
rem B3_PYTHON names the Python to use; otherwise the `py` launcher, then python on PATH.
setlocal
cd /d "%~dp0.."
if defined B3_PYTHON (
    "%B3_PYTHON%" tools\b3_launcher.py %*
    goto :done
)
where py >nul 2>nul && (
    py -3 tools\b3_launcher.py %*
    goto :done
)
where python >nul 2>nul && (
    python tools\b3_launcher.py %*
    goto :done
)
echo No Python 3 found. Install it from python.org (with Tcl/Tk), or set B3_PYTHON.
pause
:done
endlocal
