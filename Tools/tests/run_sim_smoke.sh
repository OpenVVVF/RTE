#!/usr/bin/env bash
# run_sim_smoke.sh — end-to-end smoke tests for the host simulators.
#
# Parts:
#   hostsim   emit spwm_demo_graph -> build host_sim -> run scenarios/spwm_demo.json
#             and validate the trace (rows, NaN/Inf, zero-sum currents, omega ramp).
#   plants    emit induction_vhz example graph -> build -> run the salient-PMSM
#             scenario through the spwm graph and the induction scenario through
#             the V/Hz graph; assert NaN-free bounded currents, omega ramping in
#             the commanded direction, field-locked PMSM speed and measurable
#             induction slip.
#   ngspice   run scenarios/ngspice_rl_demo.json against the real libngspice
#             backend (dlopen) with a wall-time cap that catches the old O(n^2)
#             resume regression. SKIPped (not failed) when libngspice is not
#             discoverable.
#   dcdc     run scenarios/dcdc_3bus.json through the ngspice backend in plant
#             mode "dcdc" (3-leg synchronous buck -> 3 buses), on the emitted
#             spwm-graph binary (graph writes PWM every tick, so applied
#             duties come from the graph: buses must follow applied duty*Vdc)
#             and on a graph-less base-image build (scenario "dcdc" duty keys
#             drive the legs: buses must follow those). Also runs
#             scenarios/dcdc_parallel.json (3 legs into one shared bus) and
#             checks bus voltage + leg current sharing. Same SKIP rule as
#             the ngspice part; each run is wall-capped.
#   hostsil   build Images/HostSIL (host_sil), run scenarios/sil_foc_demo.json,
#             validate with scripts/validate_trace.py.
#   rte       `rte sim --no-build` batch sanity reusing the hostsim emitted build.
#
# Runs from any cwd with no args. Scratch lives under build/ (gitignored) and
# is removed on exit unless --keep is given.
#
# usage: run_sim_smoke.sh [--only hostsim|plants|ngspice|dcdc|hostsil|rte] [--keep]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"

EMITTER="${BUILD_DIR}/bin/RTECodeEmitter"
RTE_CLI="${BUILD_DIR}/bin/rte"
HOSTSIM_SRC="${REPO_ROOT}/Images/HostSim"
HOSTSIL_SRC="${REPO_ROOT}/Images/HostSIL"

# The HostSim emit/build/rte-run trees share the fixed name below so the
# `rte sim --name ... --no-build` part reuses what the hostsim part produced.
SMOKE_NAME="spwm_demo_smoke"
EMITTED="${BUILD_DIR}/hostsim_${SMOKE_NAME}_emitted"
EMITTED_BUILD="${EMITTED}_build"
IND_EMITTED="${BUILD_DIR}/hostsim_plants_induction_emitted"
IND_EMITTED_BUILD="${IND_EMITTED}_build"
HOSTSIL_BUILD="${BUILD_DIR}/hostsil_build"

SPWM_GRAPH="${HOSTSIM_SRC}/graphs/spwm_demo_graph.json"
IND_GRAPH="${REPO_ROOT}/Assets/Examples/induction_vhz.json"
SPWM_SCENARIO_REL="scenarios/spwm_demo.json"
SALIENT_SCENARIO_REL="scenarios/salient_pmsm.json"
INDUCTION_SCENARIO_REL="scenarios/induction_vhz.json"
NGSPICE_SCENARIO_REL="scenarios/ngspice_rl_demo.json"
SIL_SCENARIO="${HOSTSIL_SRC}/scenarios/sil_foc_demo.json"
SIL_VALIDATOR="${HOSTSIL_SRC}/scripts/validate_trace.py"
DCDC_SCENARIO_REL="scenarios/dcdc_3bus.json"
DCDC_PARALLEL_SCENARIO_REL="scenarios/dcdc_parallel.json"
# Graph-less base-image build used to exercise the scenario "dcdc" duty keys
# (the emitted spwm graph writes PWM every tick and shadows them).
DCDC_BASE_BUILD="${BUILD_DIR}/hostsim_dcdc_base_build"

NGSPICE_WALL_LIMIT_S=120

ONLY=""
KEEP=0

log()  { echo "[sim-smoke] $*"; }
fail() { echo "[sim-smoke] FAIL: $*" >&2; return 1; }

usage() {
    echo "usage: $0 [--only hostsim|plants|ngspice|dcdc|hostsil|rte] [--keep]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --only)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            case "$2" in
                hostsim|plants|ngspice|dcdc|hostsil|rte) ONLY="$2" ;;
                *) usage; exit 2 ;;
            esac
            shift 2
            ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

SCRATCH="$(mktemp -d "${BUILD_DIR}/sim_smoke.XXXXXXXX")"
cleanup() {
    if [[ "${KEEP}" == "1" ]]; then
        log "--keep: scratch kept at ${SCRATCH}"
        log "--keep: emitted trees kept: ${EMITTED}{,_build} ${IND_EMITTED}{,_build} ${DCDC_BASE_BUILD}"
        return
    fi
    rm -rf "${SCRATCH}" "${EMITTED}" "${EMITTED_BUILD}" "${IND_EMITTED}" "${IND_EMITTED_BUILD}" "${DCDC_BASE_BUILD}"
}
trap cleanup EXIT

want() { [[ -z "${ONLY}" || "${ONLY}" == "$1" ]]; }

# ---------------------------------------------------------------- prerequisites

ensure_prereqs() {
    local missing=0
    [[ -x "${EMITTER}" ]] || missing=1
    [[ -x "${RTE_CLI}" ]] || missing=1
    [[ ${missing} == 1 ]] || return 0

    log "building host prerequisites (RTECodeEmitter, rte)"
    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release \
            || { echo "[sim-smoke] FAIL: cmake configure of host tools failed" >&2
                 echo "             (needs cmake, a C++20 compiler, and Qt6 — see README)" >&2
                 return 1; }
    fi
    cmake --build "${BUILD_DIR}" --target RTECodeEmitter rte --parallel "$(nproc)" \
        || { echo "[sim-smoke] FAIL: host prerequisite build failed" >&2; return 1; }
    [[ -x "${EMITTER}" && -x "${RTE_CLI}" ]] \
        || { echo "[sim-smoke] FAIL: prerequisites still missing after build" >&2; return 1; }
}

# ---------------------------------------------------------------- trace checks

# trace_check <csv> <min_data_rows> <sum_tol|-1> <omega_min|-1>
# Always checks: row count and no NaN/Inf in any column.
# sum_tol >= 0 additionally checks |i_a+i_b+i_c| <= sum_tol on every row.
# omega_min >= 0 additionally checks max(omega_e) > omega_min and that the
# final omega_e exceeds the initial one.
trace_check() {
    command -v python3 >/dev/null \
        || { echo "[sim-smoke] FAIL: python3 not found (needed for trace checks)" >&2; return 1; }
    python3 - "$@" <<'PY'
import csv
import math
import sys

path, min_rows, sum_tol, omega_min = (sys.argv[1], int(sys.argv[2]),
                                      float(sys.argv[3]), float(sys.argv[4]))
with open(path, newline="") as f:
    rows = list(csv.reader(f))
if not rows:
    print(f"FAIL: {path}: empty file")
    sys.exit(1)
header = [h.strip() for h in rows[0]]
data = rows[1:]
if len(data) < min_rows:
    print(f"FAIL: {path}: {len(data)} data rows, want >= {min_rows}")
    sys.exit(1)
try:
    ia, ib, ic = (header.index(c) for c in ("i_a", "i_b", "i_c"))
    om = header.index("omega_e")
except ValueError as e:
    print(f"FAIL: {path}: missing column {e}")
    sys.exit(1)

worst_sum = 0.0
om_max = None
for lineno, row in enumerate(data, start=2):
    if len(row) != len(header):
        print(f"FAIL: {path}:{lineno}: ragged row ({len(row)} fields)")
        sys.exit(1)
    vals = []
    for cell in row:
        try:
            v = float(cell)
        except ValueError:
            print(f"FAIL: {path}:{lineno}: non-numeric value {cell!r}")
            sys.exit(1)
        if math.isnan(v) or math.isinf(v):
            print(f"FAIL: {path}:{lineno}: NaN/Inf value {cell!r}")
            sys.exit(1)
        vals.append(v)
    if sum_tol >= 0.0:
        worst_sum = max(worst_sum, abs(vals[ia] + vals[ib] + vals[ic]))
    om_max = vals[om] if om_max is None else max(om_max, vals[om])

if sum_tol >= 0.0 and worst_sum > sum_tol:
    print(f"FAIL: {path}: max |i_a+i_b+i_c| = {worst_sum:.6g} > {sum_tol:g}")
    sys.exit(1)
if omega_min >= 0.0:
    om0, om1 = data[0][om], data[-1][om]
    if not (om_max > omega_min and om1 > om0):
        print(f"FAIL: {path}: omega_e did not ramp past {omega_min:g} rad/s "
              f"(first={float(om0):.4g} last={float(om1):.4g} max={om_max:.4g})")
        sys.exit(1)
print(f"trace ok: {path} rows={len(data)}")
PY
}

# plant_trace_check <csv> <min_data_rows> <sum_tol> <imax_bound> \
#                   <sync_elec_rad_s> <sync_tol_frac|-1> <slip_min_rad_s|-1>
# Always checks: row count, no NaN/Inf in any column, currents bounded by
# imax_bound, and that omega_e ends up moving in the commanded (positive)
# direction (final omega_e > initial + 5 rad/s).
# sync_tol_frac >= 0 additionally requires the final omega_e to sit within
# that fraction of sync_elec (synchronous machines lock to the field).
# slip_min >= 0 additionally requires sync - final >= slip_min while
# final >= 0.7*sync (induction: measurable slip, but close to sync).
plant_trace_check() {
    command -v python3 >/dev/null \
        || { echo "[sim-smoke] FAIL: python3 not found (needed for trace checks)" >&2; return 1; }
    python3 - "$@" <<'PY'
import csv
import math
import sys

(path, min_rows, sum_tol, imax_bound, sync_elec, sync_tol_frac, slip_min) = (
    sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]),
    float(sys.argv[5]), float(sys.argv[6]), float(sys.argv[7]))
with open(path, newline="") as f:
    rows = list(csv.reader(f))
if not rows:
    print(f"FAIL: {path}: empty file")
    sys.exit(1)
header = [h.strip() for h in rows[0]]
data = rows[1:]
if len(data) < min_rows:
    print(f"FAIL: {path}: {len(data)} data rows, want >= {min_rows}")
    sys.exit(1)
try:
    ia, ib, ic = (header.index(c) for c in ("i_a", "i_b", "i_c"))
    om = header.index("omega_e")
except ValueError as e:
    print(f"FAIL: {path}: missing column {e}")
    sys.exit(1)

worst_sum = 0.0
i_peak = 0.0
for lineno, row in enumerate(data, start=2):
    if len(row) != len(header):
        print(f"FAIL: {path}:{lineno}: ragged row ({len(row)} fields)")
        sys.exit(1)
    vals = []
    for cell in row:
        try:
            v = float(cell)
        except ValueError:
            print(f"FAIL: {path}:{lineno}: non-numeric value {cell!r}")
            sys.exit(1)
        if math.isnan(v) or math.isinf(v):
            print(f"FAIL: {path}:{lineno}: NaN/Inf value {cell!r}")
            sys.exit(1)
        vals.append(v)
    worst_sum = max(worst_sum, abs(vals[ia] + vals[ib] + vals[ic]))
    i_peak = max(i_peak, abs(vals[ia]), abs(vals[ib]), abs(vals[ic]))

if worst_sum > sum_tol:
    print(f"FAIL: {path}: max |i_a+i_b+i_c| = {worst_sum:.6g} > {sum_tol:g}")
    sys.exit(1)
if i_peak > imax_bound:
    print(f"FAIL: {path}: peak phase current {i_peak:.4g} A > bound {imax_bound:g}")
    sys.exit(1)
om0, om1 = float(data[0][om]), float(data[-1][om])
if sync_elec > 0.0 and not om1 > om0 + 5.0:
    print(f"FAIL: {path}: omega_e did not ramp in the commanded direction "
          f"(first={om0:.4g} last={om1:.4g})")
    sys.exit(1)
slip = sync_elec - om1
if sync_tol_frac >= 0.0 and abs(slip) > sync_tol_frac * sync_elec:
    print(f"FAIL: {path}: final omega_e {om1:.4g} rad/s not within "
          f"{sync_tol_frac:g} of sync {sync_elec:g} rad/s (slip {slip:.4g})")
    sys.exit(1)
if slip_min >= 0.0 and not (slip_min <= slip and om1 >= 0.7 * sync_elec):
    print(f"FAIL: {path}: expected measurable slip: sync={sync_elec:g} "
          f"final={om1:.4g} slip={slip:.4g} (want >= {slip_min:g} and "
          f"final >= {0.7 * sync_elec:g})")
    sys.exit(1)
print(f"plant trace ok: {path} rows={len(data)} i_peak={i_peak:.3g}A "
      f"omega_e_end={om1:.4g} slip={slip:.4g} rad/s vs sync={sync_elec:g}")
PY
}

# ---------------------------------------------------------------- hostsim core

# Emit spwm_demo_graph and build host_sim when the smoke binary is absent;
# always re-emit when the tree exists from an interrupted run missing sources.
ensure_hostsim_emitted() {
    if [[ -x "${EMITTED_BUILD}/host_sim" && -f "${EMITTED}/${SPWM_SCENARIO_REL}" ]]; then
        return 0
    fi
    log "emitting ${SPWM_GRAPH##*/} -> ${EMITTED#"${REPO_ROOT}"/}"
    rm -rf "${EMITTED}" "${EMITTED_BUILD}"
    "${EMITTER}" --base-src "${HOSTSIM_SRC}" --graph "${SPWM_GRAPH}" \
        --output "${EMITTED}" --verbosity warning \
        || { echo "[sim-smoke] FAIL: RTECodeEmitter failed" >&2; return 1; }
    log "building host_sim"
    cmake -S "${EMITTED}" -B "${EMITTED_BUILD}" \
        || { echo "[sim-smoke] FAIL: emitted cmake configure failed" >&2; return 1; }
    cmake --build "${EMITTED_BUILD}" --parallel "$(nproc)" \
        || { echo "[sim-smoke] FAIL: emitted host_sim build failed" >&2; return 1; }
}

run_hostsim_scenario() { # <scenario-rel-path> <log-file>; cwd = emitted tree
    (cd "${EMITTED}" && "${EMITTED_BUILD}/host_sim" "$1" --realtime 0) >"$2" 2>&1
}

# ---------------------------------------------------------------------- parts

part_hostsim() {
    ensure_prereqs || return 1
    ensure_hostsim_emitted || return 1

    local run_log="${SCRATCH}/hostsim_spwm_run.log"
    rm -f "${EMITTED}/trace_spwm.csv"
    run_hostsim_scenario "${SPWM_SCENARIO_REL}" "${run_log}" || {
        echo "[sim-smoke] FAIL: host_sim spwm_demo run exited nonzero; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    local trace="${EMITTED}/trace_spwm.csv"
    [[ -f "${trace}" ]] || { echo "[sim-smoke] FAIL: trace_spwm.csv not written" >&2; return 1; }
    # spwm_demo: 2.0 s at 10 kHz trace decimation -> ~20k data rows.
    trace_check "${trace}" 1000 1e-3 10.0
}

ngspice_available() {
    local ldconf
    ldconf="$(ldconfig -p 2>/dev/null)" || true
    if grep -q libngspice <<<"${ldconf}"; then
        return 0
    fi
    local d
    local IFS=':'
    for d in ${LD_LIBRARY_PATH:-}; do
        [[ -n "${d}" ]] || continue
        if compgen -G "${d}/libngspice.so*" >/dev/null; then
            return 0
        fi
    done
    return 1
}

part_ngspice() {
    if ! ngspice_available; then
        log "SKIP: libngspice not found via ldconfig or LD_LIBRARY_PATH"
        return 0
    fi
    ensure_prereqs || return 1
    ensure_hostsim_emitted || return 1

    local run_log="${SCRATCH}/hostsim_ngspice_run.log"
    local trace="${EMITTED}/ngspice_trace.csv"
    rm -f "${trace}"
    local start=${SECONDS}
    run_hostsim_scenario "${NGSPICE_SCENARIO_REL}" "${run_log}" || {
        echo "[sim-smoke] FAIL: host_sim ngspice_rl_demo run exited nonzero; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    local elapsed=$((SECONDS - start))

    if ! grep -q "libngspice loaded" "${run_log}"; then
        echo "[sim-smoke] FAIL: ngspice backend did not load; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    fi
    if grep -q "Falling back to OdePlant" "${run_log}"; then
        echo "[sim-smoke] FAIL: ngspice silently fell back to OdePlant" >&2
        return 1
    fi
    [[ ${elapsed} -lt ${NGSPICE_WALL_LIMIT_S} ]] \
        || { echo "[sim-smoke] FAIL: ngspice run took ${elapsed}s (limit ${NGSPICE_WALL_LIMIT_S}s)" >&2
             return 1; }
    [[ -f "${trace}" ]] || { echo "[sim-smoke] FAIL: ngspice_trace.csv not written" >&2; return 1; }
    # ngspice_rl_demo: 0.5 s at 10 kHz -> ~5k data rows; backend currents are
    # KCL-consistent per construction, so reuse the zero-sum check too.
    trace_check "${trace}" 1000 1e-3 -1 || return 1
    log "ngspice wall time: ${elapsed}s (limit ${NGSPICE_WALL_LIMIT_S}s)"
}

# dcdc_trace_check <csv> <min_data_rows> <vdc> <tol_frac> <fixed-duty d1 d2 d3|->
# dcdc-mode trace: probe columns v_bus1..3/i_leg1..3 present, no NaN/Inf,
# theta_e/omega_e stay 0 (no motor mechanics in dcdc mode), and the settled
# (last 20%) v_busN sits within tol_frac of dutyN%*vdc. With fixed duties
# given, the settled duty columns must equal them (scenario "dcdc" keys
# actually drove); with "-" the applied duty columns themselves are the
# reference (graph wrote PWM every tick, scenario keys shadowed).
dcdc_trace_check() {
    command -v python3 >/dev/null \
        || { echo "[sim-smoke] FAIL: python3 not found (needed for trace checks)" >&2; return 1; }
    python3 - "$@" <<'PY'
import csv
import math
import sys

(path, min_rows, vdc, tol) = (sys.argv[1], int(sys.argv[2]),
                              float(sys.argv[3]), float(sys.argv[4]))
fixed = [float(x) for x in sys.argv[5:8]] if len(sys.argv) > 5 and sys.argv[5] != "-" else None
with open(path, newline="") as f:
    rows = list(csv.reader(f))
if not rows:
    print(f"FAIL: {path}: empty file")
    sys.exit(1)
header = [h.strip() for h in rows[0]]
data = rows[1:]
if len(data) < min_rows:
    print(f"FAIL: {path}: {len(data)} data rows, want >= {min_rows}")
    sys.exit(1)
try:
    cols = {c: header.index(c) for c in (
        "duty_u", "duty_v", "duty_w", "theta_e", "omega_e",
        "v_bus1", "v_bus2", "v_bus3", "i_leg1", "i_leg2", "i_leg3")}
except ValueError as e:
    print(f"FAIL: {path}: missing column {e} (dcdc mode extends the schema "
          f"with v_bus1..3,i_leg1..3)")
    sys.exit(1)

vals = []
for lineno, row in enumerate(data, start=2):
    if len(row) != len(header):
        print(f"FAIL: {path}:{lineno}: ragged row ({len(row)} fields)")
        sys.exit(1)
    r = []
    for cell in row:
        try:
            v = float(cell)
        except ValueError:
            print(f"FAIL: {path}:{lineno}: non-numeric value {cell!r}")
            sys.exit(1)
        if math.isnan(v) or math.isinf(v):
            print(f"FAIL: {path}:{lineno}: NaN/Inf value {cell!r}")
            sys.exit(1)
        r.append(v)
    vals.append(r)

for c in ("theta_e", "omega_e"):
    i = cols[c]
    m = max(abs(r[i]) for r in vals)
    if m != 0.0:
        print(f"FAIL: {path}: {c} non-zero in dcdc mode (max {m:.4g}); "
              f"no motor mechanics should run")
        sys.exit(1)

win = vals[int(0.8 * len(vals)):]
def mean(col):
    i = cols[col]
    return sum(r[i] for r in win) / len(win)

legs = (("duty_u", "v_bus1", "i_leg1"),
        ("duty_v", "v_bus2", "i_leg2"),
        ("duty_w", "v_bus3", "i_leg3"))
out = []
for k, (dk, vk, ik) in enumerate(legs):
    d, v, i = mean(dk), mean(vk), mean(ik)
    if fixed is not None and abs(d - fixed[k]) > 0.5:
        print(f"FAIL: {path}: settled duty {k+1} = {d:.3f}% but scenario "
              f"dcdc keys say {fixed[k]:g}% (graph duty writer shadowed "
              f"them unexpectedly?)")
        sys.exit(1)
    exp = d / 100.0 * vdc
    if exp <= 0.0 or abs(v - exp) > tol * exp:
        print(f"FAIL: {path}: settled v_bus{k+1} = {v:.4g} V, want within "
              f"{tol:g} of duty*vdc = {exp:.4g} V (duty {d:.3f}%)")
        sys.exit(1)
    out.append(f"bus{k+1}={v:.3f}V/{i:.3f}A(d={d:.1f}%)")
print(f"dcdc trace ok: {path} rows={len(vals)} settled: {' '.join(out)}")
PY
}

# dcdc_parallel_check <csv> <min_data_rows> <vdc> <tol_frac> <duty> <load_ohm>
# Paralleled-leg trace: all three v_busN aliases agree, the shared bus sits
# within tol_frac of duty%*vdc, and the leg currents share the load evenly
# (each within 25% of load/3), summing to the bus current.
dcdc_parallel_check() {
    command -v python3 >/dev/null \
        || { echo "[sim-smoke] FAIL: python3 not found (needed for trace checks)" >&2; return 1; }
    python3 - "$@" <<'PY'
import csv
import math
import sys

(path, min_rows, vdc, tol, duty_pct, load) = (
    sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]),
    float(sys.argv[5]), float(sys.argv[6]))
with open(path, newline="") as f:
    rows = list(csv.reader(f))
header = [h.strip() for h in rows[0]]
data = rows[1:]
if len(data) < min_rows:
    print(f"FAIL: {path}: {len(data)} data rows, want >= {min_rows}")
    sys.exit(1)
cols = {c: header.index(c) for c in (
    "v_bus1", "v_bus2", "v_bus3", "i_leg1", "i_leg2", "i_leg3")}
vals = []
for lineno, row in enumerate(data, start=2):
    if len(row) != len(header):
        print(f"FAIL: {path}:{lineno}: ragged row"); sys.exit(1)
    r = []
    for cell in row:
        v = float(cell)
        if math.isnan(v) or math.isinf(v):
            print(f"FAIL: {path}:{lineno}: NaN/Inf value {cell!r}"); sys.exit(1)
        r.append(v)
    vals.append(r)

win = vals[int(0.8 * len(vals)):]
def mean(col):
    i = cols[col]
    return sum(r[i] for r in win) / len(win)

vb = [mean(f"v_bus{k}") for k in (1, 2, 3)]
il = [mean(f"i_leg{k}") for k in (1, 2, 3)]
exp = duty_pct / 100.0 * vdc
if abs(vb[0] - exp) > tol * exp:
    print(f"FAIL: {path}: shared bus {vb[0]:.4g} V, want within {tol:g} of "
          f"{exp:.4g} V"); sys.exit(1)
if max(vb) - min(vb) > 0.01:
    print(f"FAIL: {path}: bus aliases disagree {vb}"); sys.exit(1)
i_load = vb[0] / load
if abs(sum(il) - i_load) > 0.02 * i_load:
    print(f"FAIL: {path}: leg currents sum {sum(il):.4g} A != bus load "
          f"{i_load:.4g} A"); sys.exit(1)
i_share = i_load / 3.0
for k, i in enumerate(il):
    if abs(i - i_share) > 0.25 * i_share:
        print(f"FAIL: {path}: leg {k+1} current {i:.4g} A not within 25% of "
              f"even share {i_share:.4g} A"); sys.exit(1)
print(f"dcdc parallel ok: {path} rows={len(vals)} shared bus={vb[0]:.3f}V "
      f"legs={il[0]:.3f}/{il[1]:.3f}/{il[2]:.3f}A load={i_load:.3f}A")
PY
}

# Graph-less base-image host_sim (no generated domains) — the harness for the
# scenario-driven dcdc duty path, since bundled graphs write PWM every tick.
ensure_dcdc_base() {
    if [[ -x "${DCDC_BASE_BUILD}/host_sim" ]]; then
        return 0
    fi
    log "building base-image host_sim -> ${DCDC_BASE_BUILD#"${REPO_ROOT}"/}"
    rm -rf "${DCDC_BASE_BUILD}"
    cmake -S "${HOSTSIM_SRC}" -B "${DCDC_BASE_BUILD}" \
        || { echo "[sim-smoke] FAIL: base-image cmake configure failed" >&2; return 1; }
    cmake --build "${DCDC_BASE_BUILD}" --parallel "$(nproc)" \
        || { echo "[sim-smoke] FAIL: base-image host_sim build failed" >&2; return 1; }
}

# Check one dcdc scenario run: exit code, dcdc mode engaged, no fallback,
# wall time, trace produced. Echoes the trace path. Args: <scenario-abs> <tag>
dcdc_run_base() {
    local scenario="$1" tag="$2"
    local run_dir="${SCRATCH}/dcdc_${tag}"
    local run_log="${run_dir}/run.log"
    mkdir -p "${run_dir}"
    local start=${SECONDS}
    # Run from scratch: the trace_csv lands there (netlist resolves relative
    # to the scenario file's directory when the CWD lookup fails).
    (cd "${run_dir}" && "${DCDC_BASE_BUILD}/host_sim" "${scenario}" --realtime 0) \
        >"${run_log}" 2>&1 || {
        echo "[sim-smoke] FAIL: base host_sim ${tag} run exited nonzero; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    local elapsed=$((SECONDS - start))
    if ! grep -q "running in dcdc mode" "${run_log}"; then
        echo "[sim-smoke] FAIL: dcdc mode did not engage for ${tag}; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    fi
    if grep -qi "falling back to ode" "${run_log}"; then
        echo "[sim-smoke] FAIL: ${tag} fell back to OdePlant" >&2
        return 1
    fi
    [[ ${elapsed} -lt ${NGSPICE_WALL_LIMIT_S} ]] \
        || { echo "[sim-smoke] FAIL: ${tag} run took ${elapsed}s (limit ${NGSPICE_WALL_LIMIT_S}s)" >&2
             return 1; }
    log "${tag} wall time: ${elapsed}s (limit ${NGSPICE_WALL_LIMIT_S}s)" >&2
    echo "${run_dir}"
}

part_dcdc() {
    if ! ngspice_available; then
        log "SKIP: libngspice not found via ldconfig or LD_LIBRARY_PATH"
        return 0
    fi
    ensure_prereqs || return 1

    # --- Leg 1: emitted spwm-graph binary -----------------------------------
    # The graph's pwm.set node fires every tick, so ctx.pwm_written wins over
    # the scenario "dcdc" duty keys (documented precedence); the assertion is
    # against the *applied* duties from the trace columns.
    ensure_hostsim_emitted || return 1
    if [[ ! -f "${EMITTED}/${DCDC_SCENARIO_REL}" ]]; then
        rm -rf "${EMITTED}" "${EMITTED_BUILD}"
        ensure_hostsim_emitted || return 1
    fi
    local run_log="${SCRATCH}/dcdc_3bus_emitted.log"
    local trace="${EMITTED}/dcdc_3bus_trace.csv"
    rm -f "${trace}"
    local start=${SECONDS}
    run_hostsim_scenario "${DCDC_SCENARIO_REL}" "${run_log}" || {
        echo "[sim-smoke] FAIL: host_sim dcdc_3bus (emitted) exited nonzero; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    local elapsed=$((SECONDS - start))
    if ! grep -q "running in dcdc mode" "${run_log}"; then
        echo "[sim-smoke] FAIL: dcdc mode did not engage (emitted run); tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    fi
    if grep -qi "falling back to ode" "${run_log}"; then
        echo "[sim-smoke] FAIL: dcdc run fell back to OdePlant" >&2
        return 1
    fi
    [[ ${elapsed} -lt ${NGSPICE_WALL_LIMIT_S} ]] \
        || { echo "[sim-smoke] FAIL: dcdc emitted run took ${elapsed}s (limit ${NGSPICE_WALL_LIMIT_S}s)" >&2
             return 1; }
    [[ -f "${trace}" ]] || { echo "[sim-smoke] FAIL: dcdc_3bus_trace.csv not written" >&2; return 1; }
    dcdc_trace_check "${trace}" 1000 48.0 0.05 - || return 1
    log "dcdc_3bus (emitted, graph-driven duties) wall time: ${elapsed}s (limit ${NGSPICE_WALL_LIMIT_S}s)"

    # --- Leg 2: graph-less base image — scenario "dcdc" keys drive ----------
    ensure_dcdc_base || return 1
    local base_dir
    base_dir="$(dcdc_run_base "${HOSTSIM_SRC}/${DCDC_SCENARIO_REL}" 3bus)" || return 1
    dcdc_trace_check "${base_dir}/dcdc_3bus_trace.csv" 1000 48.0 0.05 30 20 40 || return 1

    # --- Leg 3: paralleled legs into one shared bus --------------------------
    base_dir="$(dcdc_run_base "${HOSTSIM_SRC}/${DCDC_PARALLEL_SCENARIO_REL}" parallel)" || return 1
    dcdc_parallel_check "${base_dir}/dcdc_parallel_trace.csv" 1000 48.0 0.05 30.0 2.5 || return 1
}

part_hostsil() {
    ensure_prereqs || return 1
    if [[ ! -x "${HOSTSIL_BUILD}/host_sil" ]]; then
        log "building host_sil -> ${HOSTSIL_BUILD#"${REPO_ROOT}"/}"
        # SIL_FW_SRC pinned inside the build tree so reconfigures reuse the
        # emitted firmware copy deterministically.
        cmake -S "${HOSTSIL_SRC}" -B "${HOSTSIL_BUILD}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DSIL_FW_SRC="${HOSTSIL_BUILD}/hostsil_fw_src" \
            || { echo "[sim-smoke] FAIL: HostSIL cmake configure failed" >&2; return 1; }
    fi
    cmake --build "${HOSTSIL_BUILD}" --parallel "$(nproc)" \
        || { echo "[sim-smoke] FAIL: host_sil build failed" >&2; return 1; }

    local run_dir="${SCRATCH}/hostsil_run"
    mkdir -p "${run_dir}"
    local run_log="${run_dir}/host_sil_run.log"
    (cd "${run_dir}" && "${HOSTSIL_BUILD}/host_sil" "${SIL_SCENARIO}" --realtime 0) \
        >"${run_log}" 2>&1 || {
        echo "[sim-smoke] FAIL: host_sil run exited nonzero; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    local trace="${run_dir}/sil_foc_trace.csv"
    [[ -f "${trace}" ]] || { echo "[sim-smoke] FAIL: sil_foc_trace.csv not written" >&2; return 1; }

    local val_out
    val_out="$(python3 "${SIL_VALIDATOR}" "${trace}" --control-start-s 1.6 --iq-a 8)" || {
        echo "[sim-smoke] FAIL: validate_trace.py rejected the HostSIL trace:" >&2
        echo "${val_out}" >&2
        return 1
    }
    grep -q "VALIDATION PASSED" <<<"${val_out}" \
        || { echo "[sim-smoke] FAIL: validate_trace.py output lacks VALIDATION PASSED" >&2
             return 1; }
    log "validate_trace.py: $(grep "VALIDATION PASSED" <<<"${val_out}")"
}

part_rte() {
    ensure_prereqs || return 1
    # --no-build reuses the hostsim part's emitted build tree.
    ensure_hostsim_emitted || return 1

    local run_log="${SCRATCH}/rte_sim_run.log"
    "${RTE_CLI}" sim \
        --graph "${SPWM_GRAPH}" \
        --scenario "${HOSTSIM_SRC}/${SPWM_SCENARIO_REL}" \
        --name "${SMOKE_NAME}" \
        --no-build --realtime 0 >"${run_log}" 2>&1 || {
        echo "[sim-smoke] FAIL: rte sim exited nonzero; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    local trace="${EMITTED_BUILD}/run/trace_spwm.csv"
    [[ -f "${trace}" ]] || {
        echo "[sim-smoke] FAIL: rte sim produced no trace at ${trace}" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    trace_check "${trace}" 1000 -1 -1
}

# Emits the induction_vhz example graph and builds host_sim in a dedicated
# tree; reused (like the spwm tree) across parts and runs.
ensure_induction_emitted() {
    if [[ -x "${IND_EMITTED_BUILD}/host_sim" && -f "${IND_EMITTED}/${INDUCTION_SCENARIO_REL}" ]]; then
        return 0
    fi
    log "emitting ${IND_GRAPH##*/} -> ${IND_EMITTED#"${REPO_ROOT}"/}"
    rm -rf "${IND_EMITTED}" "${IND_EMITTED_BUILD}"
    "${EMITTER}" --base-src "${HOSTSIM_SRC}" --graph "${IND_GRAPH}" \
        --output "${IND_EMITTED}" --verbosity warning \
        || { echo "[sim-smoke] FAIL: RTECodeEmitter failed (induction_vhz)" >&2; return 1; }
    log "building host_sim (induction tree)"
    cmake -S "${IND_EMITTED}" -B "${IND_EMITTED_BUILD}" \
        || { echo "[sim-smoke] FAIL: emitted cmake configure failed" >&2; return 1; }
    cmake --build "${IND_EMITTED_BUILD}" --parallel "$(nproc)" \
        || { echo "[sim-smoke] FAIL: emitted host_sim build failed" >&2; return 1; }
}

part_plants() {
    ensure_prereqs || return 1

    # --- Salient PMSM through the SPWM graph -------------------------------
    # The spwm tree predates the salient scenario in cached trees; re-emit
    # when the scenario is not in the copy.
    ensure_hostsim_emitted || return 1
    if [[ ! -f "${EMITTED}/${SALIENT_SCENARIO_REL}" ]]; then
        rm -rf "${EMITTED}" "${EMITTED_BUILD}"
        ensure_hostsim_emitted || return 1
    fi

    local run_log="${SCRATCH}/hostsim_salient_run.log"
    local trace="${EMITTED}/trace_salient_pmsm.csv"
    rm -f "${trace}"
    run_hostsim_scenario "${SALIENT_SCENARIO_REL}" "${run_log}" || {
        echo "[sim-smoke] FAIL: host_sim salient_pmsm run exited nonzero; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    [[ -f "${trace}" ]] || { echo "[sim-smoke] FAIL: trace_salient_pmsm.csv not written" >&2; return 1; }
    # throttle_b ramp ends at 0.6 -> spwm_demo_graph maps it to 1+19*0.6 =
    # 12.4 Hz electrical; the synchronous PMSM must lock to the field
    # (2*pi*12.4 = 77.9115 rad/s within 15%).
    plant_trace_check "${trace}" 1000 1e-3 25.0 77.9115 0.15 -1 || return 1

    # --- Induction machine under open-loop V/Hz -----------------------------
    ensure_induction_emitted || return 1

    run_log="${SCRATCH}/hostsim_induction_run.log"
    trace="${IND_EMITTED}/trace_induction_vhz.csv"
    rm -f "${trace}"
    (cd "${IND_EMITTED}" && "${IND_EMITTED_BUILD}/host_sim" \
        "${INDUCTION_SCENARIO_REL}" --realtime 0) >"${run_log}" 2>&1 || {
        echo "[sim-smoke] FAIL: host_sim induction_vhz run exited nonzero; tail:" >&2
        tail -n 20 "${run_log}" >&2 || true
        return 1
    }
    [[ -f "${trace}" ]] || { echo "[sim-smoke] FAIL: trace_induction_vhz.csv not written" >&2; return 1; }
    # scenarios/induction_vhz.json seeds TargetHz=40 (the graph slews 20 Hz/s,
    # 3.5 s duration -> settled at 40 Hz); sync = 2*pi*40 = 251.327 rad/s
    # (1200 rpm mechanical for pp=2). An induction machine must run measurably
    # below sync: require >= 3 rad/s slip but >= 70% of sync.
    plant_trace_check "${trace}" 1000 1e-3 20.0 251.327 -1 3.0 || return 1
}

# ----------------------------------------------------------------------- main

log "repo root: ${REPO_ROOT}"
log "scratch:   ${SCRATCH}"
[[ -n "${ONLY}" ]] && log "running only: ${ONLY}"

failures=0
for part in hostsim plants ngspice dcdc hostsil rte; do
    want "${part}" || continue
    log "=== part: ${part} ==="
    if ( set -e; "part_${part}" ); then
        log "=== part ${part}: PASS ==="
    else
        echo "[sim-smoke] === part ${part}: FAIL ===" >&2
        failures=$((failures + 1))
    fi
done

if [[ ${failures} -gt 0 ]]; then
    echo "[sim-smoke] RESULT: FAIL (${failures} part(s) failed; re-run with --keep to preserve scratch)" >&2
    exit 1
fi
log "RESULT: PASS"
