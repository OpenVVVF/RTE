#!/usr/bin/env bash
# run_sim_smoke.sh — end-to-end smoke tests for the host simulators.
#
# Parts:
#   hostsim   emit spwm_demo_graph -> build host_sim -> run scenarios/spwm_demo.json
#             and validate the trace (rows, NaN/Inf, zero-sum currents, omega ramp).
#   ngspice   run scenarios/ngspice_rl_demo.json against the real libngspice
#             backend (dlopen) with a wall-time cap that catches the old O(n^2)
#             resume regression. SKIPped (not failed) when libngspice is not
#             discoverable.
#   hostsil   build Images/HostSIL (host_sil), run scenarios/sil_foc_demo.json,
#             validate with scripts/validate_trace.py.
#   rte       `rte sim --no-build` batch sanity reusing the hostsim emitted build.
#
# Runs from any cwd with no args. Scratch lives under build/ (gitignored) and
# is removed on exit unless --keep is given.
#
# usage: run_sim_smoke.sh [--only hostsim|ngspice|hostsil|rte] [--keep]
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
HOSTSIL_BUILD="${BUILD_DIR}/hostsil_build"

SPWM_GRAPH="${HOSTSIM_SRC}/graphs/spwm_demo_graph.json"
SPWM_SCENARIO_REL="scenarios/spwm_demo.json"
NGSPICE_SCENARIO_REL="scenarios/ngspice_rl_demo.json"
SIL_SCENARIO="${HOSTSIL_SRC}/scenarios/sil_foc_demo.json"
SIL_VALIDATOR="${HOSTSIL_SRC}/scripts/validate_trace.py"

NGSPICE_WALL_LIMIT_S=120

ONLY=""
KEEP=0

log()  { echo "[sim-smoke] $*"; }
fail() { echo "[sim-smoke] FAIL: $*" >&2; return 1; }

usage() {
    echo "usage: $0 [--only hostsim|ngspice|hostsil|rte] [--keep]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --only)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            case "$2" in
                hostsim|ngspice|hostsil|rte) ONLY="$2" ;;
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
        log "--keep: emitted trees kept: ${EMITTED}{,_build}"
        return
    fi
    rm -rf "${SCRATCH}" "${EMITTED}" "${EMITTED_BUILD}"
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

# ----------------------------------------------------------------------- main

log "repo root: ${REPO_ROOT}"
log "scratch:   ${SCRATCH}"
[[ -n "${ONLY}" ]] && log "running only: ${ONLY}"

failures=0
for part in hostsim ngspice hostsil rte; do
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
