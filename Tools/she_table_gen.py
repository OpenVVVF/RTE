#!/usr/bin/env python3
"""Offline SHEPWM switching-angle solver for the Gen6 multi-modulator firmware.

Solves the three-phase selective-harmonic-elimination equations for a grid of
modulation indices and emits a C++ table header consumed by ShepwmModulator.

Waveform model (per phase leg, two-level q = +-1, quarter-wave symmetric):
  q starts at +1 at theta=0+, toggles at each of the N angles in Q1,
  Q2 mirrors Q1, second half-cycle is inverted.
  Fourier (odd harmonics only; triplens cancel in 3-phase line voltages):
    b_n / (4/pi) = 1 + 2 * sum_{i=1..N} (-1)^i * cos(n * alpha_i)
  Equations per grid point (mi in per-unit of six-step fundamental, 1.0 = square):
    fundamental:  1 + 2*sum((-1)^i cos(a_i)) = mi
    elimination:  1 + 2*sum((-1)^i cos(n*a_i)) = 0   for the first N-1
                  non-triplen odd harmonics (5, 7, 11, 13, ...)

Solving: continuation along the MI grid (previous solution seeds the next),
with random restarts at the first grid point.  Every solution is validated by
direct Fourier series evaluation before being accepted.

Usage:
  Tools/she_table_gen.py [--n 9] [--mi-min 0.50] [--mi-max 1.00] \
      [--mi-step 0.02] [--out Images/Gen6FW/Inc/Inverter/Drivers/PWM/SheTables.h]
"""

import argparse
import sys

import numpy as np
from scipy.optimize import least_squares


def harmonics_to_eliminate(count):
    """First `count` odd, non-triplen harmonics starting at 5."""
    out = []
    n = 5
    while len(out) < count:
        if n % 3 != 0:
            out.append(n)
        n += 2
    return out


def make_equations(mi, elim):
    """Residual vector f(alpha) = 0 and its analytic Jacobian."""

    def resid(alpha):
        i = np.arange(1, len(alpha) + 1)
        signs = (-1.0) ** i
        r = [1.0 + 2.0 * np.sum(signs * np.cos(alpha)) - mi]
        for n in elim:
            r.append(1.0 + 2.0 * np.sum(signs * np.cos(n * alpha)))
        return np.array(r)

    def jac(alpha):
        i = np.arange(1, len(alpha) + 1)
        signs = (-1.0) ** i
        rows = [-2.0 * signs * np.sin(alpha)]
        for n in elim:
            rows.append(-2.0 * n * signs * np.sin(n * alpha))
        return np.array(rows)

    return resid, jac


def is_valid(alpha, tol_order=1e-9):
    if np.any(alpha <= 1e-6) or np.any(alpha >= np.pi / 2 - 1e-6):
        return False
    return np.all(np.diff(alpha) > tol_order)


def solve_at(mi, elim, seed=None, rng=None, restarts=0):
    """Solve SHE equations at one MI. Returns angles or None."""
    resid, jac = make_equations(mi, elim)
    n_angles = len(elim) + 1

    guesses = []
    if seed is not None:
        guesses.append(np.array(seed, dtype=float))
    if rng is not None:
        for _ in range(restarts):
            g = np.sort(rng.uniform(0.05, np.pi / 2 - 0.05, size=n_angles))
            guesses.append(g)

    best = None
    best_cost = np.inf
    for g in guesses:
        sol = least_squares(resid, g, jac=jac, bounds=(1e-4, np.pi / 2 - 1e-4),
                            xtol=1e-15, ftol=1e-15, gtol=1e-15, max_nfev=2000)
        cost = np.max(np.abs(resid(sol.x)))
        if cost < best_cost:
            best_cost = cost
            best = sol.x
        if best_cost < 1e-9 and is_valid(best):
            break

    if best is None or best_cost > 1e-7 or not is_valid(best):
        return None
    return best


def fourier_check(alpha, n_max=121):
    """Direct Fourier magnitudes (per-unit of six-step) for odd harmonics."""
    i = np.arange(1, len(alpha) + 1)
    signs = (-1.0) ** i
    out = {}
    for n in range(1, n_max, 2):
        out[n] = abs(1.0 + 2.0 * np.sum(signs * np.cos(n * alpha)))
    return out


def solve_branch(grid, elim, rng, seed_index, restarts=300):
    """Solve across the grid by continuation from the best seed branch.

    Collects distinct solutions at grid[seed_index], continues each both
    directions along the grid, and returns the branch covering the most grid
    points as (table, covered_lo_idx, covered_hi_idx).
    """
    seed_mi = grid[seed_index]
    seeds = []
    for _ in range(restarts):
        a = solve_at(seed_mi, elim, rng=rng, restarts=1)
        if a is None:
            continue
        if all(np.max(np.abs(a - s)) > 1e-3 for s in seeds):
            seeds.append(a)
    print(f"seed mi={seed_mi:.2f}: {len(seeds)} distinct solution branches")

    best = None
    for si, s in enumerate(seeds):
        table = {seed_index: s}
        a = s
        hi = seed_index
        for k in range(seed_index + 1, len(grid)):
            nxt = solve_at(grid[k], elim, seed=a)
            if nxt is None:
                break
            table[k] = nxt
            a = nxt
            hi = k
        a = s
        lo = seed_index
        for k in range(seed_index - 1, -1, -1):
            nxt = solve_at(grid[k], elim, seed=a)
            if nxt is None:
                break
            table[k] = nxt
            a = nxt
            lo = k
        print(f"  branch {si}: covers mi={grid[lo]:.2f}..{grid[hi]:.2f} "
              f"({hi - lo + 1} points)")
        if best is None or (hi - lo) > (best[2] - best[1]):
            best = (table, lo, hi)

    return best


def emit_header(path, n_angles, mi_min, mi_step, table):
    rows = []
    for mi_idx, alpha in enumerate(table):
        vals = ", ".join(f"{a:.10f}f" for a in alpha)
        rows.append(f"    {{ {vals} }},  // mi = {mi_min + mi_idx * mi_step:.2f}")
    body = "\n".join(rows)

    content = f"""#pragma once

/* Generated by Tools/she_table_gen.py -- do not edit.
 *
 * SHEPWM switching-angle family: {n_angles} angles per quarter cycle,
 * MI grid {mi_min:.2f}..{mi_min + (len(table) - 1) * mi_step:.2f} step {mi_step:.2f}
 * (per-unit of six-step fundamental; 1.0 = square wave).
 * Angles in radians, phase-U quarter-wave; V/W derived by 120/240 deg shifts
 * and quarter/half-wave symmetry at runtime.
 *
 * MI definition: 1.0 = six-step (phase fundamental peak 2*Vdc/pi).
 * SVPWM linear max (Vdc/sqrt(3)) corresponds to MI ~ 0.907 on this scale.
 */

#include <cstdint>

namespace Inverter::shetab {{

inline constexpr uint32_t kAnglesPerQuarter = {n_angles};
inline constexpr uint32_t kMiCount = {len(table)};
inline constexpr float kMiMin = {mi_min:.6f}f;
inline constexpr float kMiStep = {mi_step:.6f}f;

inline constexpr float kAngles[kMiCount][kAnglesPerQuarter] = {{
{body}
}};

}} // namespace Inverter::shetab
"""
    with open(path, "w") as f:
        f.write(content)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n", type=int, default=9,
                    help="switching angles per quarter cycle (default 9; "
                         "eliminates harmonics 5..(first N-1 non-triplen odd))")
    ap.add_argument("--mi-min", type=float, default=0.50)
    ap.add_argument("--mi-max", type=float, default=0.90,
                    help="default 0.90: trajectory endpoint for N=9 (higher MI "
                         "needs lower-N 'gear shift' families)")
    ap.add_argument("--mi-step", type=float, default=0.02)
    ap.add_argument("--out", default="Images/Gen6FW/Inc/Inverter/Drivers/PWM/SheTables.h")
    args = ap.parse_args()

    n_angles = args.n
    elim = harmonics_to_eliminate(n_angles - 1)
    grid = np.round(np.arange(args.mi_min, args.mi_max + 0.5 * args.mi_step,
                              args.mi_step), 6)
    rng = np.random.default_rng(20260801)

    print(f"SHE solve: N={n_angles} angles/quarter, eliminating {elim}")
    print(f"MI grid: {grid[0]:.2f}..{grid[-1]:.2f} step {args.mi_step:.2f} "
          f"({len(grid)} points)")

    seed_index = int(np.argmin(np.abs(grid - 0.85)))
    table_map, lo, hi = solve_branch(grid, elim, rng, seed_index)

    if lo > 0 or hi < len(grid) - 1:
        print(f"FAIL: best branch covers mi={grid[lo]:.2f}..{grid[hi]:.2f}, "
              f"not the full requested grid; adjust --mi-min/--mi-max.")
        sys.exit(1)

    table = [table_map[k] for k in range(len(grid))]

    # Validate every grid point before emitting.
    for k, alpha in enumerate(table):
        spec = fourier_check(alpha)
        worst_elim = max(spec[n] for n in elim)
        uncontrolled = [n for n in sorted(spec)
                        if n not in elim and n % 3 != 0 and n > 1]
        first_un = uncontrolled[0] if uncontrolled else 0
        print(f"  mi={grid[k]:.2f}  fund={spec[1]:.4f}  "
              f"worst_elim={worst_elim:.2e}  "
              f"first uncontrolled h{first_un}="
              f"{spec.get(first_un, 0) / max(first_un, 1):.3f} pu")
        if abs(spec[1] - grid[k]) > 1e-6 or worst_elim > 1e-6:
            print(f"FAIL: validation mismatch at mi={grid[k]:.2f}")
            sys.exit(1)

    emit_header(args.out, n_angles, float(grid[0]), float(args.mi_step), table)
    print(f"Wrote {args.out}: {len(table)} MI points x {n_angles} angles")


if __name__ == "__main__":
    main()
