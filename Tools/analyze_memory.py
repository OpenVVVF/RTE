#!/usr/bin/env python3
"""Analyze STM32H7 firmware memory layout from an ELF file.

Reports per-region usage (DTCMRAM, RAM_D1, RAM_D2, RAM_D3) and attributes the
largest symbols / object files to each region.  Can enforce a budget so the
build fails before DTCMRAM overflows.
"""

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass(frozen=True)
class MemoryRegion:
    name: str
    origin: int
    length: int


@dataclass
class Symbol:
    address: int
    size: int
    name: str
    section: str
    object_file: Optional[str]


# Memory layout for STM32H723ZGTx as described by the linker script.
DEFAULT_REGIONS = [
    MemoryRegion("DTCMRAM", 0x2000_0000, 128 * 1024),
    MemoryRegion("RAM_D1", 0x2400_0000, 320 * 1024),
    MemoryRegion("RAM_D2", 0x3000_0000, 32 * 1024),
    MemoryRegion("RAM_D3", 0x3800_0000, 16 * 1024),
    MemoryRegion("ITCMRAM", 0x0000_0000, 64 * 1024),
]


def run(cmd: List[str]) -> str:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed: {result.stderr}")
    return result.stdout


def parse_sections(elf: Path) -> Dict[str, Tuple[int, int]]:
    """Return {section_name: (address, size)} from readelf."""
    out = run(["arm-none-eabi-readelf", "-S", str(elf)])
    sections: Dict[str, Tuple[int, int]] = {}
    # Section Headers:
    #   [Nr] Name              Type            Addr     Off    Size   ES Flg Lk Inf Al
    for line in out.splitlines():
        m = re.match(
            r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)\s+\S+\s+([0-9a-fA-F]+)",
            line,
        )
        if m:
            sections[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return sections


def region_for_address(address: int, regions: List[MemoryRegion]) -> Optional[MemoryRegion]:
    for r in regions:
        if r.origin <= address < r.origin + r.length:
            return r
    return None


def parse_map_address_to_object(map_path: Path) -> Dict[int, str]:
    """Build an {address: object_file} index from a GNU linker map.

    The "Linker script and memory map" section contains input-section
    contributions of the form:

        .data._ZN...s_instanceE
                        0x20001214     0x206c CMakeFiles/.../EncoderADC.cpp.obj

    We record the start address of each contribution and attribute every nm
    symbol at that address to the listed object file.  Address-based lookup is
    required because static file-scope "s_instance" variables in different
    translation units all mangled to the same name.
    """
    index: Dict[int, str] = {}
    if not map_path.exists():
        return index

    text = map_path.read_text(errors="replace")
    marker = "Linker script and memory map"
    start = text.find(marker)
    if start < 0:
        return index

    section_line: Optional[str] = None
    for line in text[start:].splitlines():
        stripped = line.strip()
        if not stripped:
            continue

        if section_line is None:
            if re.match(r"^\.[A-Za-z0-9_.]+$", stripped):
                section_line = stripped
            continue

        # Typical line: 0x20001214     0x206c CMakeFiles/.../EncoderADC.cpp.obj
        # Sometimes:   0x20000000      0x20 *fill*
        m = re.match(r"^\s*0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+)$", stripped)
        if m:
            addr = int(m.group(1), 16)
            rest = m.group(3).strip()
            if not rest.startswith("*fill*"):
                index[addr] = rest

        section_line = None
        if re.match(r"^\.[A-Za-z0-9_.]+$", stripped):
            section_line = stripped

    return index


def parse_symbols(elf: Path, map_path: Optional[Path] = None) -> List[Symbol]:
    """Parse all data/bss symbols with sizes from nm and object-file attribution."""
    out = run(["arm-none-eabi-nm", "-S", "--size-sort", "--radix=x", str(elf)])
    object_index = parse_map_address_to_object(map_path) if map_path else {}
    symbols: List[Symbol] = []
    for line in out.splitlines():
        # Format: <addr> <size> <type> <name> [<object>]
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            addr = int(parts[0], 16)
            size = int(parts[1], 16)
        except ValueError:
            continue
        sym_type = parts[2]
        if sym_type not in "dDbBcC":
            continue
        name = parts[3]
        obj = object_index.get(addr)
        symbols.append(
            Symbol(
                address=addr,
                size=size,
                name=name,
                section=".data" if sym_type in "dD" else ".bss",
                object_file=obj,
            )
        )
    return symbols


def classify_symbols(
    symbols: List[Symbol], regions: List[MemoryRegion]
) -> Dict[str, List[Symbol]]:
    buckets: Dict[str, List[Symbol]] = {r.name: [] for r in regions}
    buckets["OTHER"] = []
    for sym in symbols:
        region = region_for_address(sym.address, regions)
        if region:
            buckets[region.name].append(sym)
        else:
            buckets["OTHER"].append(sym)
    return buckets


def object_name(path: Optional[str]) -> str:
    if not path:
        return "<unknown>"
    p = Path(path)
    name = p.name
    # Strip CMake's .obj suffix and the long CMakeFiles prefix for readability.
    if name.endswith(".obj"):
        name = name[:-4]
    if "CMakeFiles" in p.parts:
        # Try to keep Src/... path context.
        try:
            idx = p.parts.index("CMakeFiles")
            rel_parts = p.parts[idx + 2:]  # skip CMakeFiles/<target>.dir
            if rel_parts:
                return str(Path(*rel_parts).with_suffix(""))
        except ValueError:
            pass
    return name


def format_size(n: int) -> str:
    return f"{n:>8,} B ({n / 1024:>7.2f} KB)"


def print_report(
    buckets: Dict[str, List[Symbol]],
    regions: List[MemoryRegion],
    top_n: int,
    warn_threshold: int,
) -> Dict[str, int]:
    totals: Dict[str, int] = {}
    for region in regions:
        syms = buckets.get(region.name, [])
        total = sum(s.size for s in syms)
        totals[region.name] = total
        pct = 100.0 * total / region.length
        bar = "█" * int(pct / 5) + "░" * (20 - int(pct / 5))
        print(f"\n{region.name} ({region.length / 1024:.0f} KB): {bar} {pct:5.1f}%  {format_size(total)}")

        if total == 0:
            continue

        by_file: Dict[str, int] = {}
        for s in syms:
            by_file[object_name(s.object_file)] = by_file.get(object_name(s.object_file), 0) + s.size
        print(f"  Top object files:")
        for obj, size in sorted(by_file.items(), key=lambda x: -x[1])[:top_n]:
            print(f"    {format_size(size)}  {obj}")

        print(f"  Top symbols:")
        for s in sorted(syms, key=lambda x: -x.size)[:top_n]:
            marker = " ⚠" if s.size >= warn_threshold else ""
            obj_hint = f" [{object_name(s.object_file)}]" if s.object_file else ""
            print(f"    {format_size(s.size)}  {s.section:<5} {s.name}{obj_hint}{marker}")

    other_total = sum(s.size for s in buckets.get("OTHER", []))
    if other_total:
        print(f"\nOTHER (outside known regions): {format_size(other_total)}")
        for s in sorted(buckets["OTHER"], key=lambda x: -x.size)[:top_n]:
            print(f"    {format_size(s.size)}  {s.section:<5} {s.name}")

    return totals


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze STM32H7 firmware memory usage")
    parser.add_argument("elf", type=Path, help="Path to firmware ELF")
    parser.add_argument(
        "--map",
        type=Path,
        default=None,
        help="Path to linker map file (defaults to <elf-stem>.map next to the ELF)",
    )
    parser.add_argument(
        "--budget-dtcm",
        type=int,
        default=118 * 1024,
        help="Fail if DTCMRAM usage exceeds this many bytes (default 118 KB, leaves 10 KB headroom)",
    )
    parser.add_argument(
        "--warn-symbol",
        type=int,
        default=1024,
        help="Flag individual symbols larger than this many bytes (default 1 KB)",
    )
    parser.add_argument("--top", type=int, default=10, help="Show top N symbols/object files per region")
    args = parser.parse_args()

    if not args.elf.exists():
        print(f"error: ELF not found: {args.elf}", file=sys.stderr)
        return 1

    map_path = args.map
    if map_path is None:
        candidate = args.elf.with_suffix(".map")
        if not candidate.exists():
            candidate = args.elf.parent / f"{args.elf.stem}.map"
        map_path = candidate if candidate.exists() else None

    symbols = parse_symbols(args.elf, map_path)
    buckets = classify_symbols(symbols, DEFAULT_REGIONS)
    totals = print_report(buckets, DEFAULT_REGIONS, args.top, args.warn_symbol)

    dtcm = totals.get("DTCMRAM", 0)
    if dtcm > args.budget_dtcm:
        print(
            f"\nERROR: DTCMRAM usage {dtcm} B exceeds budget {args.budget_dtcm} B "
            f"by {dtcm - args.budget_dtcm} B",
            file=sys.stderr,
        )
        return 2

    print(f"\nOK: DTCMRAM usage {dtcm} B is within budget {args.budget_dtcm} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
