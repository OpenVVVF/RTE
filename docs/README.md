# RTE Documentation

This tree documents the **OpenVVVF/RTE** toolchain: the node-graph model, the
Qt node editor (`NodeGUI`), code generation, firmware emission, and the path
from a graph JSON file to a flashable STM32 binary.

Hardware designs, HARA/TARA/SWAD, and the chassis user manual live in the
sibling repo [OpenVVVF/Hardware](https://github.com/OpenVVVF/Hardware)
(`Docs/`). Firmware, Real Time Examiner host tooling, and node-based codegen
live here.

## Guides

| Document | Contents |
|---|---|
| [Getting Started](GettingStarted.md) | Clone, dependencies, build host tools, run `NodeGUI` |
| [NodeGUI](NodeGUI.md) | Node editor UI: open/save graphs, ports, domains, bridges, parameters |
| [Toolchain](Toolchain.md) | End-to-end pipeline: graph → codegen → emit → firmware binary |
| [Node Templates](NodeTemplates.md) | How `Assets/NodeTemplates` work and how to add a new node type |

## Library / tool READMEs

| Path | Role |
|---|---|
| [`Lib/NodeAPI/README.md`](../Lib/NodeAPI/README.md) | Graph model, wire types, timing validation, JSON |
| [`Lib/InverterCodegen/README.md`](../Lib/InverterCodegen/README.md) | Graph → per-domain C++ |
| [`Lib/InverterProtocol/`](../Lib/InverterProtocol/) | Host/device telemetry + command packets |
| [`Source/NodeGUI/README.md`](../Source/NodeGUI/README.md) | NodeGUI build notes |
| [`Source/RTECodeEmitter/README.md`](../Source/RTECodeEmitter/README.md) | Marker-based insertion into a base firmware tree |
| [`Source/RTEFirmwareBuilder/README.md`](../Source/RTEFirmwareBuilder/README.md) | ARM build wrapper |

## Related hardware docs

Safety and product documentation remain in Hardware:

- `Docs/HARA.md` / `HARA.pdf` — hazard analysis & fault injection
- `Docs/TARA.md` / `TARA.pdf` — threat analysis
- `Docs/SWAD.md` / `SWAD.pdf` — software architecture (RTE config tool referenced there)
- `Docs/Manual.md` / `Traction_Inverter_User_Manual.pdf` — user manual
