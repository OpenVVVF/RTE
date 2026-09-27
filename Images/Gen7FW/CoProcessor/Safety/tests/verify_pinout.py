#!/usr/bin/env python3
"""Check the firmware's safety pin assumptions against Chassis2 KiCad XML netlists."""

from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[5]
HARDWARE = ROOT.parent / "InverterGen5/Hardware/Chassis2/Boards"


def export_nets(schematic: Path) -> dict[str, list[tuple[str, str]]]:
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "netlist.xml"
        subprocess.run(
            ["kicad-cli", "sch", "export", "netlist", "--format", "kicadxml",
             "-o", str(output), str(schematic)],
            check=True,
        )
        tree = ET.parse(output)
    return {
        net.attrib["name"]: [
            (node.attrib["ref"], node.attrib.get("pinfunction", node.attrib["pin"]))
            for node in net.findall("node")
        ]
        for net in tree.findall(".//nets/net")
    }


def require_pin(nets: dict[str, list[tuple[str, str]]], name: str,
                ref: str, pin_prefix: str) -> None:
    matches = [pins for net, pins in nets.items() if net == name]
    assert len(matches) == 1, f"missing or ambiguous net {name}"
    assert any(r == ref and pin.split("_")[0] == pin_prefix
               for r, pin in matches[0]), f"{ref}.{pin_prefix} not on {name}"


def main() -> None:
    board = export_nets(HARDWARE / "ControlBoard/ControlBoard.kicad_sch")
    expected = {
        "/GATE_DRIVE_PWR2_ENABLE": "PC7",
        "/GATE_DRIVE_PWR2_FEEDBACK": "PC15",
        "/GATE_DRIVE_PWR1_FEEDBACK": "PC14",
        "/FAULT": "PB10",
        "/READY": "PC6",
        "/DRIVER_RESET": "PA10",
        "/Control/Main Processor/COPROCESSOR_SYNC": "PB9",
        "/Control/Main Processor/COPROCESSOR_WAKEUP": "PC13",
        "/Control/Main Processor/USART3_RX": "PC10",
        "/Control/Main Processor/USART3_TX": "PC11",
        "/Control/SN_THROTTLE_A": "PC1",
        "/Control/SN_THROTTLE_B": "PC2",
        "/Control/SN_CUR_SENSE_PH_U_SIG": "PA0",
        "/Control/SN_CUR_SENSE_PH_V_SIG": "PA2",
        "/Control/SN_CUR_SENSE_PH_W_SIG": "PA6",
        "/Control/SN_CUR_SENSE_DC_LINK_SIG": "PC4",
    }
    for net, pin in expected.items():
        require_pin(board, net, "U9", pin)
    require_pin(board, "/DRIVER_RESET", "U1", "PD5")
    require_pin(board, "/Control/Main Processor/COPROCESSOR_SYNC", "U1", "PD8")
    require_pin(board, "/Control/Main Processor/COPROCESSOR_WAKEUP", "U1", "PD9")
    require_pin(board, "/Control/Main Processor/USART3_RX", "U1", "PB11")
    require_pin(board, "/Control/Main Processor/USART3_TX", "U1", "PB10")
    require_pin(board, "/GATE_DRIVE_PWR1_ENABLE", "U1", "PC10")

    # Catch a drift between the checked schematic and CubeMX-generated GPIO
    # definitions; the firmware adapter uses these labels directly.
    header = (ROOT / "Images/Gen7FW/CoProcessor/Core/Inc/main.h").read_text()
    firmware_labels = {
        "/GATE_DRIVE_PWR2_ENABLE": "GATE_DRIVE_PWR_ENABLE",
        "/GATE_DRIVE_PWR2_FEEDBACK": "GATE_DRIVE_PWR2_FEEDBACK",
        "/GATE_DRIVE_PWR1_FEEDBACK": "GATE_DRIVE_PWR1_FEEDBACK",
        "/FAULT": "GATE_DRIVER_FAULT_IN",
        "/READY": "GATE_DRIVE_READY",
        "/DRIVER_RESET": "GATE_DRIVER_RESET",
        "/Control/Main Processor/COPROCESSOR_SYNC": "INTERMCU_SYNC_LINE",
    }
    for net, label in firmware_labels.items():
        pin = expected[net]
        port, number = pin[:2], pin[2:]
        assert re.search(rf"#define {label}_Pin GPIO_PIN_{number}\b", header), label
        assert re.search(rf"#define {label}_GPIO_Port GPIO{port[1]}\b", header), label

    main_header = (ROOT / "Images/Gen7FW/MainProcessor/Inc/main.h").read_text()
    assert "#define COPROCESSOR_SYNC_Pin GPIO_PIN_8" in main_header
    assert "#define COPROCESSOR_SYNC_GPIO_Port GPIOD" in main_header
    assert "#define COPROCESSOR_WAKEUP_Pin GPIO_PIN_9" in main_header
    assert "#define COPROCESSOR_WAKEUP_GPIO_Port GPIOD" in main_header
    assert "#define GATE_DRIVER_POWER_ENABLE_Pin GPIO_PIN_10" in main_header
    assert "#define GATE_DRIVER_POWER_ENABLE_GPIO_Port GPIOC" in main_header

    gate = export_nets(HARDWARE / "GateDriver/GateDriver.kicad_sch")
    require_pin(gate, "/GATE_DRIVE_PWR1_ENABLE", "U5", "IN")
    require_pin(gate, "/GATE_DRIVE_PWR2_ENABLE", "U6", "IN")
    series = [pins for pins in gate.values()
              if ("U5", "OUT_5") in pins and ("U6", "Vbb_3") in pins]
    assert len(series) == 1, "PWR1 and PWR2 gate switches are not in series"
    # The feedback pins must actually be driven from the two switch outputs.
    # R28/R29 and R27/R30 form 3k/1k dividers; R31/R32 feed the MCU nets.
    assert all(pair in series[0] for pair in (("R28", "1"),))
    assert all(pair in gate["Net-(Z1-K)"] for pair in
               (("R28", "2"), ("R31", "1"), ("R27", "1")))
    assert ("R31", "2") in gate["/GATE_DRIVE_PWR1_FEEDBACK"]
    assert all(pair in gate["/+12V_A"] for pair in
               (("U6", "OUT_5"), ("R29", "1")))
    assert all(pair in gate["Net-(Z2-K)"] for pair in
               (("R29", "2"), ("R32", "1"), ("R30", "1")))
    assert ("R32", "2") in gate["/GATE_DRIVE_PWR2_FEEDBACK"]
    print("Chassis2 coprocessor pinout, power switches, and feedback verified")


if __name__ == "__main__":
    main()
