#!/usr/bin/env python3
"""Generate Assets/Examples/induction_vhz.json from node templates + custom nodes."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEMPLATES = ROOT / "Assets" / "NodeTemplates"
OUT = ROOT / "Assets" / "Examples" / "induction_vhz.json"

DT = 0.0002  # 5 kHz TIM1 update ISR in FOC mode


def load_template(category_name: str) -> dict:
    d = TEMPLATES / category_name
    meta = json.loads((d / "node.json").read_text())
    inline_file = d / "inline.cpp"
    if inline_file.exists():
        meta["inlineCode"] = inline_file.read_text()
    # Ensure the GUI-emitted keys exist.
    meta.setdefault("classDefinition", "")
    meta.setdefault("classHeader", "")
    meta.setdefault("constructorCode", "")
    meta.setdefault("displayName", meta.get("id", "").split(".")[-1])
    meta.setdefault("domain", "")
    meta.setdefault("defaultName", meta.get("displayName", ""))
    meta.setdefault("description", "")
    return meta


def make_custom_types() -> list[dict]:
    return [
        {
            "id": "induction.AngleIntegrator",
            "displayName": "Angle Integrator",
            "defaultName": "AngleIntegrator",
            "description": "Integrates electrical frequency in Hz to a wrapped angle in radians.",
            "domain": "",
            "isEntryPoint": False,
            "maxInstances": 0,
            "classDefinition": "",
            "classHeader": "",
            "constructorCode": "",
            "inputPorts": [
                {
                    "name": "Hz",
                    "description": "Electrical frequency in Hz.",
                    "direction": "input",
                    "type": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"},
                }
            ],
            "outputPorts": [
                {
                    "name": "Theta",
                    "description": "Wrapped electrical angle in radians.",
                    "direction": "output",
                    "type": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"},
                }
            ],
            "parameterTypes": {
                "Dt": {
                    "description": "Update period in seconds.",
                    "quantity": "dimensionless",
                    "frame": "scalar",
                    "dtype": "f32",
                },
                "Angle": {
                    "description": "Persistent integrated angle state in radians.",
                    "quantity": "dimensionless",
                    "frame": "scalar",
                    "dtype": "f32",
                },
            },
            "inlineCode": (
                "Angle += Hz * 6.28318530718f * Dt;\n"
                "while (Angle >= 6.28318530718f) Angle -= 6.28318530718f;\n"
                "while (Angle < 0.0f) Angle += 6.28318530718f;\n"
                "Theta = Angle;\n"
            ),
        },
        {
            "id": "induction.VHzToAlphaBeta",
            "displayName": "V/Hz to Alpha/Beta",
            "defaultName": "VHzToAlphaBeta",
            "description": "Scalar V/Hz law: converts frequency and angle to alpha-beta voltage vector.",
            "domain": "",
            "isEntryPoint": False,
            "maxInstances": 0,
            "classDefinition": "",
            "classHeader": "",
            "constructorCode": "",
            "inputPorts": [
                {
                    "name": "Hz",
                    "description": "Electrical frequency in Hz.",
                    "direction": "input",
                    "type": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"},
                },
                {
                    "name": "Theta",
                    "description": "Electrical angle in radians.",
                    "direction": "input",
                    "type": {"quantity": "dimensionless", "frame": "scalar", "dtype": "f32"},
                },
            ],
            "outputPorts": [
                {
                    "name": "V_Alpha",
                    "description": "Stationary-frame alpha-axis voltage.",
                    "direction": "output",
                    "type": {"quantity": "voltage", "frame": "scalar", "dtype": "f32"},
                },
                {
                    "name": "V_Beta",
                    "description": "Stationary-frame beta-axis voltage.",
                    "direction": "output",
                    "type": {"quantity": "voltage", "frame": "scalar", "dtype": "f32"},
                },
            ],
            "parameterTypes": {
                "VoltsPerHz": {
                    "description": "V/Hz ratio in peak line-neutral volts per hertz.",
                    "quantity": "dimensionless",
                    "frame": "scalar",
                    "dtype": "f32",
                },
                "BoostVolts": {
                    "description": "Voltage boost added at all frequencies for low-speed torque.",
                    "quantity": "dimensionless",
                    "frame": "scalar",
                    "dtype": "f32",
                },
            },
            "inlineCode": (
                "float vmag = fabsf(Hz) * VoltsPerHz + BoostVolts;\n"
                "if (vmag < 0.0f) vmag = 0.0f;\n"
                "V_Alpha = rte::Volts(vmag * cosf(Theta));\n"
                "V_Beta  = rte::Volts(vmag * sinf(Theta));\n"
            ),
        },
    ]


def make_graph() -> dict:
    standard = [
        "Values.Var",
        "Values.Config",
        "Control.Slew",
        "Sensors.DcLinkVoltage",
        "Transforms.Svpwm",
        "Actuators.PwmOut",
        "Debug.TelemetryLog",
    ]
    node_types = make_custom_types()
    for t in standard:
        node_types.append(load_template(t))

    nodes = [
        # Frequency command + ramp
        {
            "id": "TargetHz",
            "type": "Values.Var",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {"Stored": "0.0", "Set": "0.0", "In": "0.0"},
        },
        {
            "id": "CfgSlewRate",
            "type": "Values.Config",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {
                "Cached": "0.0",
                "DefaultValue": "20.0",
                "Key": "Induction.SlewRateHzPs",
            },
        },
        {
            "id": "FreqSlew",
            "type": "Control.Slew",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameterInputs": ["Rate"],
            "parameters": {"Dt": str(DT), "Rate": "20.0", "Value": "0.0"},
        },
        # V/Hz tuning constants
        {
            "id": "CfgVoltsPerHz",
            "type": "Values.Config",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {
                "Cached": "0.0",
                "DefaultValue": "1.6",
                "Key": "Induction.VoltsPerHz",
            },
        },
        {
            "id": "CfgBoostVolts",
            "type": "Values.Config",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {
                "Cached": "0.0",
                "DefaultValue": "0.0",
                "Key": "Induction.BoostVolts",
            },
        },
        # Angle + V/Hz vector generation
        {
            "id": "AngleGen",
            "type": "induction.AngleIntegrator",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {"Dt": str(DT), "Angle": "0.0"},
        },
        {
            "id": "VHzGen",
            "type": "induction.VHzToAlphaBeta",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameterInputs": ["VoltsPerHz", "BoostVolts"],
            "parameters": {"VoltsPerHz": "1.6", "BoostVolts": "0.0"},
        },
        # DC link + modulation
        {
            "id": "VdcSense",
            "type": "Sensors.DcLinkVoltage",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {},
        },
        {
            "id": "Svpwm",
            "type": "Transforms.Svpwm",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {},
        },
        {
            "id": "PwmOut",
            "type": "Actuators.PwmOut",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {},
        },
        # Telemetry
        {
            "id": "LogFreq",
            "type": "Debug.TelemetryLog",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {"Key": "ind_hz"},
        },
        {
            "id": "LogVmag",
            "type": "Debug.TelemetryLog",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {"Key": "ind_vmag"},
        },
        {
            "id": "LogAngle",
            "type": "Debug.TelemetryLog",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {"Key": "ind_theta"},
        },
        {
            "id": "LogVdc",
            "type": "Debug.TelemetryLog",
            "domain": "tim_isr",
            "position": {"x": 0, "y": 0},
            "parameters": {"Key": "ind_vdc"},
        },
    ]

    connections = [
        # TargetHz -> Slew
        {
            "id": "c_target_to_slew",
            "from": {"nodeId": "TargetHz", "portName": "Value"},
            "to": {"nodeId": "FreqSlew", "portName": "In"},
        },
        # Config -> Slew Rate param
        {
            "id": "c_cfg_slew",
            "from": {"nodeId": "CfgSlewRate", "portName": "Value"},
            "to": {"nodeId": "FreqSlew", "portName": "Rate"},
        },
        # Slew -> Angle + VHz
        {
            "id": "c_slew_to_angle",
            "from": {"nodeId": "FreqSlew", "portName": "Out"},
            "to": {"nodeId": "AngleGen", "portName": "Hz"},
        },
        {
            "id": "c_slew_to_vhz",
            "from": {"nodeId": "FreqSlew", "portName": "Out"},
            "to": {"nodeId": "VHzGen", "portName": "Hz"},
        },
        # Angle -> VHz
        {
            "id": "c_angle_to_vhz",
            "from": {"nodeId": "AngleGen", "portName": "Theta"},
            "to": {"nodeId": "VHzGen", "portName": "Theta"},
        },
        # VHz -> SVPWM
        {
            "id": "c_valpha",
            "from": {"nodeId": "VHzGen", "portName": "V_Alpha"},
            "to": {"nodeId": "Svpwm", "portName": "V_Alpha"},
        },
        {
            "id": "c_vbeta",
            "from": {"nodeId": "VHzGen", "portName": "V_Beta"},
            "to": {"nodeId": "Svpwm", "portName": "V_Beta"},
        },
        # Vdc -> SVPWM
        {
            "id": "c_vdc",
            "from": {"nodeId": "VdcSense", "portName": "V_Dc"},
            "to": {"nodeId": "Svpwm", "portName": "V_Dc"},
        },
        # SVPWM -> PWM out
        {
            "id": "c_duty_a",
            "from": {"nodeId": "Svpwm", "portName": "Duty_A"},
            "to": {"nodeId": "PwmOut", "portName": "Duty_A"},
        },
        {
            "id": "c_duty_b",
            "from": {"nodeId": "Svpwm", "portName": "Duty_B"},
            "to": {"nodeId": "PwmOut", "portName": "Duty_B"},
        },
        {
            "id": "c_duty_c",
            "from": {"nodeId": "Svpwm", "portName": "Duty_C"},
            "to": {"nodeId": "PwmOut", "portName": "Duty_C"},
        },
        # Config -> V/Hz params
        {
            "id": "c_cfg_vhz_ratio",
            "from": {"nodeId": "CfgVoltsPerHz", "portName": "Value"},
            "to": {"nodeId": "VHzGen", "portName": "VoltsPerHz"},
        },
        {
            "id": "c_cfg_vhz_boost",
            "from": {"nodeId": "CfgBoostVolts", "portName": "Value"},
            "to": {"nodeId": "VHzGen", "portName": "BoostVolts"},
        },
        # Telemetry taps
        {
            "id": "c_log_freq",
            "from": {"nodeId": "FreqSlew", "portName": "Out"},
            "to": {"nodeId": "LogFreq", "portName": "Value"},
        },
        {
            "id": "c_log_vmag",
            "from": {"nodeId": "VHzGen", "portName": "V_Alpha"},
            "to": {"nodeId": "LogVmag", "portName": "Value"},
        },
        {
            "id": "c_log_angle",
            "from": {"nodeId": "AngleGen", "portName": "Theta"},
            "to": {"nodeId": "LogAngle", "portName": "Value"},
        },
        {
            "id": "c_log_vdc",
            "from": {"nodeId": "VdcSense", "portName": "V_Dc"},
            "to": {"nodeId": "LogVdc", "portName": "Value"},
        },
    ]

    return {
        "name": "induction_vhz",
        "nodeTypes": node_types,
        "nodes": nodes,
        "connections": connections,
        "bridges": [],
    }


if __name__ == "__main__":
    graph = make_graph()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(graph, indent=2) + "\n")
    print(f"Wrote {OUT}")
