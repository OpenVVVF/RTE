"""
rteplot_lib.py — shared helpers for reading RTE .jsonl telemetry logs and
building polished Plotly figures.

This is intentionally plain Python + pandas/plotly so future recipes are easy to
write: a recipe is just a module with a ``make_fig(df, meta, cfg) -> go.Figure``
function.
"""
from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable

import numpy as np
import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots


@dataclass
class SessionMeta:
    """Lightweight metadata extracted from the session_start record."""

    source: Path
    started_at: str = ""
    exported_at: str = ""
    port: str = ""
    mode: str = ""
    protocol: str = ""
    telemetry_count: int = 0
    duration_s: float = 0.0
    signals: list[str] = field(default_factory=list)

    def title_line(self) -> str:
        base = self.source.stem
        if self.started_at:
            return f"{base} — {self.started_at}"
        return base


@dataclass
class PlotConfig:
    """User-facing configuration passed to every recipe."""

    max_points: int = 5_000
    time_window: tuple[float | None, float | None] = (None, None)
    width_px: int = 1280
    height_px_per_row: int = 260
    logo_path: Path | None = None
    logo_scale: float = 0.18
    show_legend: bool = True
    palette: list[str] = field(default_factory=lambda: [
        "#003c6c",  # UCSC navy-ish
        "#f29813",  # warm accent
        "#13a665",  # green
        "#d64550",  # red
        "#6c4c9d",  # purple
        "#00848e",  # teal
        "#5f6b7d",  # slate
    ])
    template: str = "plotly_white"


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def parse_jsonl(path: Path) -> tuple[SessionMeta, pd.DataFrame]:
    """Parse an RTE .jsonl file into metadata and a tidy DataFrame."""

    path = Path(path)
    meta = SessionMeta(source=path)
    records: list[dict[str, Any]] = []
    signal_set: set[str] = set()

    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue

            if obj.get("type") == "session_start":
                meta.started_at = obj.get("started_at_utc", "")
                meta.exported_at = obj.get("exported_at_utc", "")
                meta.port = obj.get("port", "")
                meta.mode = obj.get("mode", "")
                meta.protocol = obj.get("protocol", "")
                continue

            if obj.get("type") != "telemetry":
                continue

            vals = obj.get("values", {})
            row = {"t": float(obj.get("t", 0.0))}
            for k, v in vals.items():
                if v is None:
                    continue
                try:
                    row[k] = float(v)
                    signal_set.add(k)
                except (TypeError, ValueError):
                    pass
            records.append(row)

    if not records:
        return meta, pd.DataFrame()

    df = pd.DataFrame.from_records(records)
    df = df.sort_values("t").reset_index(drop=True)
    meta.telemetry_count = len(df)
    meta.duration_s = float(df["t"].iloc[-1] - df["t"].iloc[0])
    meta.signals = sorted(signal_set)
    return meta, df


def apply_window(df: pd.DataFrame, cfg: PlotConfig) -> pd.DataFrame:
    """Crop the DataFrame to the configured time window."""

    t0, t1 = cfg.time_window
    if t0 is not None:
        df = df[df["t"] >= t0]
    if t1 is not None:
        df = df[df["t"] <= t1]
    return df.reset_index(drop=True)


def downsample(df: pd.DataFrame, max_points: int) -> pd.DataFrame:
    """Largest-Triangle-Three-Buckets (LTTB) downsampling.

    Keeps visual shape far better than naive decimation while being O(n).
    """

    n = len(df)
    if n <= max_points or max_points <= 2:
        return df

    data = df.to_numpy(dtype=float)
    sampled = np.empty((max_points, data.shape[1]))
    sampled[0] = data[0]
    sampled[-1] = data[-1]

    # Remaining points form (max_points - 2) buckets.
    bucket_size = (n - 2) / (max_points - 2)

    for i in range(max_points - 2):
        start = int((i + 1) * bucket_size) + 1
        end = int((i + 2) * bucket_size) + 1
        end = min(end, n - 1)

        if start >= end:
            # Degenerate tiny bucket: take its midpoint.
            sampled[i + 1] = data[(start + end) // 2]
            continue

        # Average of the *next* bucket (or the last point for the final bucket).
        next_start = end
        next_end = min(int((i + 3) * bucket_size) + 1, n)
        if next_start >= next_end:
            avg_next = data[-1]
        else:
            avg_next = data[next_start:next_end].mean(axis=0)

        # Point in current bucket that maximises triangle area with previous
        # selected point (a) and avg_next.
        a = sampled[i]
        bucket = data[start:end]
        # Triangle area is proportional to the magnitude of the cross product.
        # Vector a->avg_next and a->point. Score across all value columns.
        base = avg_next - a
        vecs = bucket - a
        # Use norm of projection perpendicular to base as proxy for area.
        base_norm_sq = np.dot(base, base)
        if base_norm_sq == 0:
            proj = np.zeros_like(vecs)
        else:
            proj = (vecs @ base)[:, None] * base / base_norm_sq
        perp = vecs - proj
        scores = np.square(perp).sum(axis=1)
        best = start + int(np.argmax(scores))
        sampled[i + 1] = data[best]

    return pd.DataFrame(sampled, columns=df.columns)


# ---------------------------------------------------------------------------
# Plotting primitives
# ---------------------------------------------------------------------------

def apply_base_layout(
    fig: go.Figure,
    meta: SessionMeta,
    cfg: PlotConfig,
    title: str,
    subtitle: str = "",
) -> go.Figure:
    """Apply shared branding, fonts, and metadata annotation."""

    fig.update_layout(
        title=dict(
            text=f"<b>{title}</b><br><sup>{subtitle or meta.title_line()}</sup>",
            x=0.0,
            xanchor="left",
            font=dict(size=20, color="#1a1a1a"),
        ),
        font=dict(family="Arial, Helvetica, sans-serif", size=12, color="#333333"),
        template=cfg.template,
        legend=dict(
            orientation="h",
            yanchor="bottom",
            y=-0.22,
            xanchor="left",
            x=0.0,
            bgcolor="rgba(255,255,255,0.7)",
        ),
        margin=dict(l=70, r=50, t=90, b=90),
        hovermode="x unified",
        showlegend=cfg.show_legend,
    )

    # Footer with session metadata
    footer_parts = [
        f"samples: {meta.telemetry_count:,}",
        f"duration: {meta.duration_s:.3f}s",
        f"mode: {meta.mode}",
        f"port: {meta.port}",
    ]
    if meta.exported_at:
        footer_parts.append(f"exported: {meta.exported_at}")
    fig.add_annotation(
        text=" | ".join(p for p in footer_parts if p),
        xref="paper", yref="paper",
        x=1.0, y=-0.16,
        showarrow=False,
        font=dict(size=9, color="#666666"),
        xanchor="right",
    )

    # Optional institutional logo
    if cfg.logo_path and cfg.logo_path.exists():
        fig.add_layout_image(
            dict(
                source=str(cfg.logo_path),
                xref="paper", yref="paper",
                x=1.0, y=1.0,
                xanchor="right", yanchor="top",
                sizex=cfg.logo_scale, sizey=cfg.logo_scale,
                sizing="contain",
                opacity=0.85,
                layer="above",
            )
        )

    return fig


def make_subplot_fig(
    df: pd.DataFrame,
    signals: list[str],
    meta: SessionMeta,
    cfg: PlotConfig,
    title: str,
    subtitle: str = "",
    secondary_y: dict[str, int] | None = None,
    hlines: dict[str, float] | None = None,
) -> go.Figure:
    """Stack one signal per row, optionally grouping secondary_y signals."""

    if secondary_y is None:
        secondary_y = {}

    # Build rows: primary signals first, secondary ones appended to their host row
    primary = [s for s in signals if secondary_y.get(s, -1) == -1]
    n_rows = len(primary)
    row_of_signal: dict[str, int] = {}
    for i, s in enumerate(primary):
        row_of_signal[s] = i + 1

    # map secondary signals to same row as their primary host
    for sec, host_idx in secondary_y.items():
        if sec in signals and 1 <= host_idx <= n_rows:
            row_of_signal[sec] = host_idx

    fig = make_subplots(
        rows=n_rows,
        cols=1,
        shared_xaxes=True,
        vertical_spacing=0.06,
        subplot_titles=primary,
    )

    t = df["t"].to_numpy()
    color_cycle = cfg.palette

    for s in signals:
        if s not in df.columns:
            continue
        row = row_of_signal.get(s)
        if row is None:
            continue
        color = color_cycle[(signals.index(s) if s in signals else 0) % len(color_cycle)]
        yaxis = f"y{row}" if row > 1 else "y"
        fig.add_trace(
            go.Scatter(
                x=t,
                y=df[s].to_numpy(),
                mode="lines",
                name=s,
                line=dict(color=color, width=1.4),
                hovertemplate=f"{s}: %{{y:.4g}}<extra></extra>",
            ),
            row=row, col=1,
        )

    # Reference lines
    if hlines:
        for s, val in hlines.items():
            if s not in row_of_signal:
                continue
            row = row_of_signal[s]
            fig.add_hline(
                y=val,
                row=row, col=1,
                line=dict(color="#d64550", width=1.5, dash="dash"),
                annotation_text=f"{val:.3g}",
                annotation_position="right",
            )

    fig.update_xaxes(title_text="time (s)", row=n_rows, col=1)
    height = max(360, n_rows * cfg.height_px_per_row)
    fig.update_layout(height=height, width=cfg.width_px)
    apply_base_layout(fig, meta, cfg, title, subtitle)
    return fig


def pick_signal(df: pd.DataFrame, *candidates: str) -> str | None:
    """Return the first candidate signal that exists in the DataFrame."""

    for c in candidates:
        if c in df.columns:
            return c
    return None


def pick_signals(df: pd.DataFrame, groups: list[list[str]]) -> list[str]:
    """For each group of candidates, return the first present signal."""

    return [s for s in (pick_signal(df, *g) for g in groups) if s is not None]


def add_modulation_index(df: pd.DataFrame) -> pd.DataFrame:
    """Compute useful derived quantities if the right raw signals exist."""

    if "m_idx" in df.columns:
        return df

    vd = pick_signal(df, "cg_vd_v", "mpcc_vd_v")
    vq = pick_signal(df, "cg_vq_v", "mpcc_vq_v")
    vdc = pick_signal(df, "vdc_v", "cg_vdc_v", "mpcc_vdc_v")

    if vd and vq and vdc:
        vdc_s = df[vdc].replace(0, np.nan)
        df = df.copy()
        df["m_idx"] = np.sqrt(df[vd] ** 2 + df[vq] ** 2) / (vdc_s / math.sqrt(3))
    return df


# ---------------------------------------------------------------------------
# Anomaly detection
# ---------------------------------------------------------------------------


@dataclass
class AnomalySegment:
    """A contiguous time interval where a diagnostic metric is out of bounds."""

    kind: str
    t0: float
    t1: float
    severity: float
    detail: str = ""


def _merge_segments(
    times: np.ndarray,
    mask: np.ndarray,
    min_width_s: float = 0.05,
) -> list[tuple[float, float]]:
    """Return merged contiguous time intervals where mask is True."""

    segments: list[tuple[float, float]] = []
    in_seg = False
    t0 = 0.0
    for t, m in zip(times, mask):
        if m and not in_seg:
            in_seg = True
            t0 = t
        elif not m and in_seg:
            if t - t0 >= min_width_s:
                segments.append((t0, t))
            in_seg = False
    if in_seg:
        if times[-1] - t0 >= min_width_s:
            segments.append((t0, float(times[-1])))
    return segments


def find_anomalies(df: pd.DataFrame) -> list[AnomalySegment]:
    """Scan a telemetry DataFrame and return a list of suspicious intervals."""

    anomalies: list[AnomalySegment] = []
    if df.empty:
        return anomalies

    t = df["t"].to_numpy()
    rpm = pick_signal(df, "Mech_RPM", "Elec_RPM", "RPM")
    id_a = pick_signal(df, "cg_id_a", "mpcc_id_a")
    iq_a = pick_signal(df, "cg_iq_a", "mpcc_iq_a")
    vq_v = pick_signal(df, "cg_vq_v", "mpcc_vq_v")
    vd_v = pick_signal(df, "cg_vd_v", "mpcc_vd_v")
    vdc = pick_signal(df, "vdc_v", "cg_vdc_v", "mpcc_vdc_v")

    # Modulation index / overmodulation
    df = add_modulation_index(df)
    if "m_idx" in df.columns:
        m = df["m_idx"].to_numpy()
        for t0, t1 in _merge_segments(t, m > 1.0, min_width_s=0.1):
            peak = float(df[(df["t"] >= t0) & (df["t"] <= t1)]["m_idx"].max())
            anomalies.append(
                AnomalySegment(
                    kind="overmodulation",
                    t0=t0,
                    t1=t1,
                    severity=peak,
                    detail=f"peak m_idx = {peak:.3f}",
                )
            )

    # Voltage saturation: vq near its own ceiling for a sustained period
    if vq_v:
        vq = df[vq_v].to_numpy()
        vq_max = np.nanmax(vq)
        if vq_max > 1.0:
            plateau_mask = vq > vq_max * 0.95
            for t0, t1 in _merge_segments(t, plateau_mask, min_width_s=0.2):
                anomalies.append(
                    AnomalySegment(
                        kind="voltage_saturation",
                        t0=t0,
                        t1=t1,
                        severity=float(vq_max),
                        detail=f"{vq_v} clamped near {vq_max:.2f} V",
                    )
                )

    # Current oscillation: high std in id or iq inside rolling windows
    for sig, label in [(id_a, "id_oscillation"), (iq_a, "iq_oscillation")]:
        if sig is None:
            continue
        x = df[sig].to_numpy()
        roll = pd.Series(x).rolling(window=50, min_periods=10).std().to_numpy()
        mask = (~np.isnan(roll)) & (roll > 3.0)
        for t0, t1 in _merge_segments(t, mask, min_width_s=0.2):
            peak_std = float(np.nanmax(roll[(t >= t0) & (t <= t1)]))
            anomalies.append(
                AnomalySegment(
                    kind=label,
                    t0=t0,
                    t1=t1,
                    severity=peak_std,
                    detail=f"{sig} rolling std = {peak_std:.2f} A",
                )
            )

    # Field-weakening failure: at high speed id should be negative, but it isn't
    if rpm and id_a:
        rpm_arr = df[rpm].to_numpy()
        id_arr = df[id_a].to_numpy()
        rpm_threshold = np.nanmax(rpm_arr) * 0.6
        high_speed = rpm_arr > rpm_threshold
        for t0, t1 in _merge_segments(t, high_speed & (id_arr > 1.0), min_width_s=0.3):
            anomalies.append(
                AnomalySegment(
                    kind="fw_id_positive",
                    t0=t0,
                    t1=t1,
                    severity=float(np.nanmax(id_arr[(t >= t0) & (t <= t1)])),
                    detail=f"{id_a} > 0 A while {rpm} > {rpm_threshold:.0f} RPM",
                )
            )

    return sorted(anomalies, key=lambda a: a.t0)


def summarize_anomalies(df: pd.DataFrame) -> dict[str, Any]:
    """Return a concise numerical summary of the log's health."""

    summary: dict[str, Any] = {"duration_s": 0.0, "signals": list(df.columns)}
    if df.empty:
        return summary

    t = df["t"].to_numpy()
    summary["duration_s"] = float(t[-1] - t[0])
    summary["samples"] = len(df)

    rpm = pick_signal(df, "Mech_RPM", "Elec_RPM", "RPM")
    if rpm:
        summary["rpm_max"] = float(df[rpm].max())
        summary["rpm_min"] = float(df[rpm].min())

    df = add_modulation_index(df)
    if "m_idx" in df.columns:
        m = df["m_idx"].to_numpy()
        summary["m_idx_max"] = float(np.nanmax(m))
        summary["m_idx_over_1_pct"] = float(np.mean(m > 1.0) * 100)

    id_a = pick_signal(df, "cg_id_a", "mpcc_id_a")
    if id_a:
        summary["id_min"] = float(df[id_a].min())
        summary["id_max"] = float(df[id_a].max())
        summary["id_std"] = float(df[id_a].std())

    iq_a = pick_signal(df, "cg_iq_a", "mpcc_iq_a")
    if iq_a:
        summary["iq_min"] = float(df[iq_a].min())
        summary["iq_max"] = float(df[iq_a].max())
        summary["iq_std"] = float(df[iq_a].std())

    return summary


# ---------------------------------------------------------------------------
# Recipe loading
# ---------------------------------------------------------------------------

def load_recipe(name: str) -> Callable[..., go.Figure]:
    """Import a recipe module by name from the ``recipes`` package."""

    import importlib

    mod = importlib.import_module(f"recipes.{name}")
    if not hasattr(mod, "make_fig"):
        raise ValueError(f"recipe '{name}' does not define make_fig(df, meta, cfg)")
    return mod.make_fig


def list_recipes() -> list[str]:
    """Return names of available recipe modules."""

    here = Path(__file__).parent / "recipes"
    return sorted(
        p.stem
        for p in here.glob("*.py")
        if p.stem != "__init__" and not p.stem.startswith("_")
    )
