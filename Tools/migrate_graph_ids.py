#!/usr/bin/env python3
"""Rewrite node type ids in NodeAPI graph JSON files to the new
PascalCase.Name library scheme (node-library-overhaul).

Usage: python3 Tools/migrate_graph_ids.py <graph.json> [...]
The ID_MAP is importable for other tooling.
"""

import json
import os
import sys
from collections import OrderedDict

ID_MAP = {
    # consolidations
    "hw.adc.phase_currents": "Sensors.PhaseCurrents",
    "hw.phase_current_reader": "Sensors.PhaseCurrents",
    "hw.encoder_angle": "Sensors.Encoder",
    "hw.encoder.decode": "Sensors.Encoder",
    "control.pi": "Control.Pi",
    "control.pi_current": "Control.Pi",
    "var.float": "Values.Var",
    "var.current": "Values.Var",
    "constant.current": "Values.Constant",
    # plain renames
    "hw.dc_link_voltage": "Sensors.DcLinkVoltage",
    "hw.phase_voltages": "Sensors.PhaseVoltages",
    "hw.temperatures": "Sensors.Temperatures",
    "hw.throttle": "Sensors.Throttle",
    "hw.digital_in": "Sensors.DigitalIn",
    "hw.digital_out": "Actuators.DigitalOut",
    "hw.can_rx": "Sensors.CanRx",
    "hw.can_tx": "Actuators.CanTx",
    "hw.pwm.set_duty": "Actuators.PwmOut",
    "math.clarke": "Transforms.Clarke",
    "math.park": "Transforms.Park",
    "math.inverse_clarke": "Transforms.InverseClarke",
    "math.inverse_park": "Transforms.InversePark",
    "math.sincos": "Transforms.SinCos",
    "math.svpwm": "Transforms.Svpwm",
    "math.encoder_elec_angle": "Transforms.ElecAngle",
    "math.forced_angle": "Transforms.ForcedAngle",
    "math.greater": "Logic.Greater",
    "math.less": "Logic.Less",
    "math.mux": "Logic.Mux",
    "control.gate": "Logic.Gate",
    "control.slew": "Control.Slew",
    "var.bool": "Values.VarBool",
    "config.value": "Values.Config",
    "app.telemetry_log": "Debug.TelemetryLog",
    "app.telemetry_current_sink": "Debug.TelemetryCurrentSink",
}


# Ids that are merges of several old types: embedded nodeTypes with these ids
# are replaced with the canonical template content (older embedded variants
# can carry mismatched ports/maxInstances/forced domains).
MERGE_TARGETS = {
    "Sensors.PhaseCurrents", "Sensors.Encoder", "Control.Pi",
    "Values.Var", "Values.Constant",
}


def load_template(templates_dir, tid):
    with open(os.path.join(templates_dir, tid, "node.json")) as f:
        t = json.load(f, object_pairs_hook=OrderedDict)
    with open(os.path.join(templates_dir, tid, "inline.cpp")) as f:
        code = f.read()
    out = OrderedDict()
    for k in ("classDefinition", "classHeader", "constructorCode"):
        out[k] = ""
    out["displayName"] = t.get("displayName", tid)
    out["domain"] = t.get("domain", "")
    out["id"] = t["id"]
    out["inlineCode"] = code
    out["inputPorts"] = t.get("inputPorts", [])
    out["isEntryPoint"] = t.get("isEntryPoint", False)
    out["maxInstances"] = t.get("maxInstances", 0)
    out["outputPorts"] = t.get("outputPorts", [])
    out["parameterTypes"] = t.get("parameterTypes", {})
    return out


def migrate(path, write=True, templates_dir=None):
    with open(path) as f:
        g = json.load(f, object_pairs_hook=OrderedDict)

    changed = 0
    for node in g.get("nodes", []):
        old_type = node.get("type")
        new = ID_MAP.get(old_type)
        if new:
            node["type"] = new
            changed += 1

        if old_type in ("hw.adc.phase_currents", "hw.phase_current_reader"):
            params = node.setdefault("parameters", OrderedDict())
            params.setdefault(
                "InvertPolarity",
                "1.0" if old_type == "hw.adc.phase_currents" else "0.0",
            )
            changed += 1
        elif node.get("type") == "Sensors.PhaseCurrents":
            params = node.setdefault("parameters", OrderedDict())
            if "InvertPolarity" not in params:
                params["InvertPolarity"] = (
                    "1.0" if node.get("domain") == "adc_isr" else "0.0"
                )
                changed += 1

        if old_type == "constant.current":
            params = node.get("parameters", {})
            if "Amps" in params:
                params["Value"] = params.pop("Amps")
                changed += 1
    for nt in g.get("nodeTypes", []):
        new = ID_MAP.get(nt.get("id"))
        if new:
            nt["id"] = new
            changed += 1

    if templates_dir:
        # Canonicalize merged types: drop all embedded variants and insert the
        # template-dir content once.
        have = {nt.get("id") for nt in g.get("nodeTypes", [])}
        g["nodeTypes"] = [nt for nt in g.get("nodeTypes", [])
                          if nt.get("id") not in MERGE_TARGETS]
        for tid in sorted(MERGE_TARGETS & (have | {n["type"] for n in g["nodes"]})):
            try:
                g["nodeTypes"].append(load_template(templates_dir, tid))
                changed += 1
            except FileNotFoundError:
                print(f"  ! template missing for {tid}, keeping stale embedded copy")

    if write and changed:
        with open(path, "w") as f:
            json.dump(g, f, indent=2)
            f.write("\n")
    return changed


if __name__ == "__main__":
    # Resolve the template dir relative to the repo root (this script lives in
    # Tools/), not the caller's cwd, so template canonicalization always runs.
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    templates_dir = os.path.join(repo_root, "Assets", "NodeTemplates")
    total = 0
    for path in sys.argv[1:]:
        n = migrate(path, templates_dir=templates_dir)
        total += n
        print(f"{path}: {n} ids rewritten")
    sys.exit(0 if total or len(sys.argv) > 1 else 1)
