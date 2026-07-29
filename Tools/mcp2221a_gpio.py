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
"""
import sys
import time


def get_device():
    try:
        from EasyMCP2221 import Device
    except ImportError as e:
        raise ImportError(
            "EasyMCP2221 not installed. "
            "Run: python3 -m venv .venv && .venv/bin/pip install EasyMCP2221"
        ) from e
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
