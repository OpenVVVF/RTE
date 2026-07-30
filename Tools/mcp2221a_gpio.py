#!/usr/bin/env python3
"""
MCP2221A GPIO helper for the RTE firmware updater.

Commands:
    enter   - Assert BOOT0=HIGH, pulse NRST, then wait for bootloader.
    exit    - De-assert BOOT0=LOW, pulse NRST to run application.
    release - Release GP0/GP1 to high-Z inputs.

Expected wiring (same as flash_uart.py):
    GP0 -> BOOT0 (PH3)
    GP1 -> NRST

If EasyMCP2221 is unavailable, the helper bootstraps a project-local .venv
and installs it automatically. On Linux systems that do not include Python
virtual-environment support, a desktop authorization prompt is used before
attempting to install the required OS package.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


SCRIPT_PATH = Path(__file__).resolve()
PROJECT_ROOT = SCRIPT_PATH.parent.parent
VENV_DIR = PROJECT_ROOT / ".venv"
VENV_PYTHON = VENV_DIR / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def _run(command, description):
    print(f"[EasyMCP2221 setup] {description}", flush=True)
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"{description} failed (exit code {e.returncode})") from e
    except OSError as e:
        raise RuntimeError(f"{description} failed: {e}") from e


def _venv_can_import():
    if not VENV_PYTHON.is_file():
        return False
    result = subprocess.run(
        [str(VENV_PYTHON), "-c", "import EasyMCP2221"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def _linux_distribution():
    values = {}
    try:
        for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            key, separator, value = line.partition("=")
            if separator:
                values[key] = value.strip().strip("\"'")
    except OSError:
        pass
    return values.get("ID", ""), values.get("ID_LIKE", "")


def _venv_support_install_command():
    """Return the safest known command for adding venv/ensurepip support."""
    if not sys.platform.startswith("linux"):
        return None

    distro_id, distro_like = _linux_distribution()
    distro_names = {distro_id, *distro_like.split()}

    if distro_names & {"debian", "ubuntu"} and shutil.which("apt-get"):
        return ["apt-get", "install", "-y", "python3-venv"]
    if distro_names & {"fedora", "rhel", "centos"} and shutil.which("dnf"):
        return ["dnf", "install", "-y", "python3-pip"]
    return None


def _request_venv_support_install():
    package_command = _venv_support_install_command()
    if package_command is None:
        return False

    if shutil.which("pkexec"):
        _run(
            ["pkexec", *package_command],
            "requesting administrator approval for Python environment support",
        )
        return True

    # A terminal launch can use sudo's normal prompt. Do not try this from a
    # detached GUI, where sudo cannot safely ask the user for a password.
    if sys.stdin.isatty() and shutil.which("sudo"):
        _run(
            ["sudo", *package_command],
            "requesting administrator approval for Python environment support",
        )
        return True

    return False


def _create_venv():
    creation_error = None
    try:
        _run(
            [sys.executable, "-m", "venv", str(VENV_DIR)],
            f"creating local Python environment at {VENV_DIR}",
        )
        return
    except RuntimeError as first_error:
        creation_error = first_error
        print(f"[EasyMCP2221 setup] {first_error}", flush=True)

    venv_support = subprocess.run(
        [sys.executable, "-c", "import ensurepip, venv"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if venv_support.returncode == 0:
        raise creation_error

    if not _request_venv_support_install():
        raise RuntimeError(
            "Python virtual-environment support is unavailable and no supported "
            "administrator prompt could be opened. Install the Python venv package "
            "for this operating system, then retry."
        )

    _run(
        [sys.executable, "-m", "venv", str(VENV_DIR)],
        f"retrying local Python environment creation at {VENV_DIR}",
    )


def _bootstrap_easymcp2221():
    if _venv_can_import():
        print(
            f"[EasyMCP2221 setup] Using existing dependency from {VENV_DIR}",
            flush=True,
        )
    else:
        if not VENV_PYTHON.is_file():
            _create_venv()

        # A partially-created environment may have Python but no pip.
        pip_check = subprocess.run(
            [str(VENV_PYTHON), "-m", "pip", "--version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if pip_check.returncode != 0:
            try:
                _run(
                    [str(VENV_PYTHON), "-m", "ensurepip", "--upgrade"],
                    "installing pip in the local Python environment",
                )
            except RuntimeError:
                if not _request_venv_support_install():
                    raise
                _run(
                    [str(VENV_PYTHON), "-m", "ensurepip", "--upgrade"],
                    "retrying pip installation in the local Python environment",
                )

        _run(
            [str(VENV_PYTHON), "-m", "pip", "install", "EasyMCP2221"],
            "installing EasyMCP2221 in the local Python environment",
        )

        if not _venv_can_import():
            raise RuntimeError(
                f"EasyMCP2221 was installed but cannot be imported by {VENV_PYTHON}"
            )

    # The current process is still the interpreter that lacked the package.
    # Replace it so imports and USB dependencies come from the repaired venv.
    os.execv(
        str(VENV_PYTHON),
        [str(VENV_PYTHON), str(SCRIPT_PATH), *sys.argv[1:]],
    )


def get_device():
    try:
        from EasyMCP2221 import Device
    except ImportError:
        print(
            "[EasyMCP2221 setup] Dependency is missing; attempting automatic repair.",
            flush=True,
        )
        _bootstrap_easymcp2221()
        raise RuntimeError("EasyMCP2221 repair returned without restarting")
    return Device()


def cmd_enter():
    mcp = get_device()
    # Configure as outputs: BOOT0=LOW, RESET=HIGH (not in reset).
    mcp.set_pin_function(gp0="GPIO_OUT", gp1="GPIO_OUT", out0=False, out1=True)
    time.sleep(0.01)

    # BOOT0 high, reset still released: let it settle.
    mcp.GPIO_write(gp0=True, gp1=True)
    time.sleep(0.05)

    # Assert reset.
    mcp.GPIO_write(gp0=True, gp1=False)
    time.sleep(0.05)

    # Release reset and give the H7 ROM bootloader time to stabilize.
    mcp.GPIO_write(gp0=True, gp1=True)
    time.sleep(0.25)
    print("bootloader entered")


def cmd_exit():
    mcp = get_device()
    mcp.set_pin_function(gp0="GPIO_OUT", gp1="GPIO_OUT", out0=False, out1=True)
    time.sleep(0.01)

    # BOOT0 low, assert reset.
    mcp.GPIO_write(gp0=False, gp1=False)
    time.sleep(0.05)

    # Release reset to run application.
    mcp.GPIO_write(gp1=True)
    time.sleep(0.1)
    print("application started")


def cmd_release():
    try:
        mcp = get_device()
        mcp.set_pin_function(gp0="GPIO_IN", gp1="GPIO_IN", out0=False, out1=False)
        print("released")
    except Exception as e:
        print(f"release warning: {e}")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} enter|exit|release")
        sys.exit(1)

    command = sys.argv[1].lower()
    try:
        if command == "enter":
            cmd_enter()
        elif command == "exit":
            cmd_exit()
        elif command == "release":
            cmd_release()
        else:
            print(f"Unknown command: {command}")
            sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
