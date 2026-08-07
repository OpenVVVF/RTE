"""
Field-weakening anomaly-analysis recipe.

This is a more diagnostic version of ``fieldweakening``. It highlights the
specific failure signatures that matter for field-weakening tuning:

* sustained overmodulation (m_idx > 1)
* noisy / oscillating id current
* voltage saturation (vq clamped)
* iq collapse or instability at high speed

It also adds vertical shaded bands for the high-speed region and annotates the
worst overmodulation peak.
"""
from __future__ import annotations

import numpy as np
import plotly.graph_objects as go

import rteplot_lib as core
from rteplot_lib import PlotConfig, SessionMeta

TITLE = "Field-Weakening Anomaly Analysis"
DESCRIPTION = (
    "Diagnostic view highlighting overmodulation, id instability, and voltage "
    "saturation during field-weakening operation."
)


def make_fig(df, meta: SessionMeta, cfg: PlotConfig):
    df = core.add_modulation_index(df)

    rpm = core.pick_signal(df, "Mech_RPM", "Elec_RPM", "RPM")
    id_a = core.pick_signal(df, "cg_id_a", "mpcc_id_a")
    iq_a = core.pick_signal(df, "cg_iq_a", "mpcc_iq_a")
    vd_v = core.pick_signal(df, "cg_vd_v", "mpcc_vd_v")
    vq_v = core.pick_signal(df, "cg_vq_v", "mpcc_vq_v")
    vdc = core.pick_signal(df, "vdc_v", "cg_vdc_v", "mpcc_vdc_v")

    omega = core.pick_signal(df, "mpcc_omega_e")
    if rpm is None and omega:
        df = df.copy()
        df["_omega_rpm"] = df[omega] * 60.0 / (2 * 3.141592653589793)
        rpm = "_omega_rpm"

    has_midx = "m_idx" in df.columns and df["m_idx"].notna().any()
    signals = [s for s in [rpm, id_a, iq_a, vd_v, vq_v, vdc, "m_idx" if has_midx else None] if s]

    fig = core.make_subplot_fig(
        df=df,
        signals=signals,
        meta=meta,
        cfg=cfg,
        title=TITLE,
        subtitle=meta.source.name,
        hlines={"m_idx": 1.0} if "m_idx" in signals else None,
    )

    # Determine high-speed region (RPM > 60% of max) for shading.
    if rpm and rpm in df.columns:
        rpm_max = df[rpm].max()
        rpm_threshold = rpm_max * 0.6
        high_speed = df[df[rpm] > rpm_threshold]
        if not high_speed.empty:
            t_start = high_speed["t"].min()
            t_end = high_speed["t"].max()
            for row in range(1, len(signals) + 1):
                fig.add_vrect(
                    x0=t_start,
                    x1=t_end,
                    row=row,
                    col=1,
                    fillcolor="#003c6c",
                    opacity=0.04,
                    line_width=0,
                )
            fig.add_annotation(
                text="high-speed / FW region",
                xref="x", yref="paper",
                x=(t_start + t_end) / 2,
                y=1.0,
                showarrow=False,
                font=dict(size=10, color="#003c6c"),
                xanchor="center",
                yanchor="top",
            )

    # Shade overmodulation region and annotate worst peak.
    if "m_idx" in df.columns and not df["m_idx"].empty:
        m_max = df["m_idx"].max()
        if m_max > 1.0:
            midx_row = signals.index("m_idx") + 1
            fig.add_hrect(
                y0=1.0,
                y1=m_max * 1.05,
                row=midx_row,
                col=1,
                fillcolor="#d64550",
                opacity=0.10,
                line_width=0,
            )
            peak_t = df.loc[df["m_idx"].idxmax(), "t"]
            fig.add_annotation(
                text=f"peak m_idx = {m_max:.3f}",
                xref="x", yref=f"y{midx_row}" if midx_row > 1 else "y",
                x=peak_t,
                y=m_max,
                showarrow=True,
                arrowhead=2,
                arrowsize=1,
                arrowwidth=1,
                font=dict(size=10, color="#d64550"),
                ax=40,
                ay=-40,
            )

    # Axis units
    for s, unit in [(id_a, "A"), (iq_a, "A"), (vd_v, "V"), (vq_v, "V"), (vdc, "V")]:
        if s and s in signals:
            fig.update_yaxes(title_text=unit, row=signals.index(s) + 1, col=1)

    if rpm and rpm in signals:
        fig.update_yaxes(title_text="RPM", row=signals.index(rpm) + 1, col=1)

    # Add a small note about expected id behavior.
    if id_a and id_a in signals:
        id_row = signals.index(id_a) + 1
        fig.add_annotation(
            text="expected: stable negative id during FW",
            xref="paper", yref=f"y{id_row}" if id_row > 1 else "y",
            x=0.02,
            y=0.95,
            showarrow=False,
            font=dict(size=9, color="#666666"),
            xanchor="left",
            yanchor="top",
        )

    return fig
