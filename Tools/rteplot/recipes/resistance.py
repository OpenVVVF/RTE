"""
Resistance-calibration telemetry view: phase currents, DC bus, and measured
line-to-line / phase resistance values.
"""
from __future__ import annotations

import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots

import rteplot_lib as core
from rteplot_lib import PlotConfig, SessionMeta

TITLE = "Resistance Calibration"
DESCRIPTION = "Phase currents, DC bus voltage, and measured resistances."


def make_fig(df: pd.DataFrame, meta: SessionMeta, cfg: PlotConfig):
    # Pull signals (with fallbacks)
    iu = core.pick_signal(df, "cg_iu_a", "mpcc_iu_a")
    iv = core.pick_signal(df, "cg_iv_a", "mpcc_iv_a")
    iw = core.pick_signal(df, "cg_iw_a", "mpcc_iw_a")
    vdc = core.pick_signal(df, "vdc_v", "cg_vdc_v")
    rll_uv = core.pick_signal(df, "r_ll_uv")
    rll_uw = core.pick_signal(df, "r_ll_uw")
    rll_vw = core.pick_signal(df, "r_ll_vw")
    r_phase_avg = core.pick_signal(df, "r_phase_avg", "motor_r_phase_avg")

    signals = [s for s in [iu, iv, iw, vdc, rll_uv, rll_uw, rll_vw, r_phase_avg] if s]

    fig = make_subplots(
        rows=3,
        cols=1,
        shared_xaxes=True,
        vertical_spacing=0.08,
        subplot_titles=("Phase Currents", "DC Bus Voltage", "Measured Resistance"),
    )

    t = df["t"]

    # Row 1: currents
    for s, color in zip([iu, iv, iw], cfg.palette[:3]):
        if s:
            fig.add_trace(
                go.Scatter(
                    x=t,
                    y=df[s],
                    mode="lines",
                    name=s,
                    line=dict(color=color, width=1.2),
                    hovertemplate="%{y:.3f} A<extra>" + s + "</extra>",
                ),
                row=1,
                col=1,
            )

    # Row 2: Vdc
    if vdc:
        fig.add_trace(
            go.Scatter(
                x=t,
                y=df[vdc],
                mode="lines",
                name=vdc,
                line=dict(color=cfg.palette[3], width=1.2),
                hovertemplate="%{y:.2f} V<extra>" + vdc + "</extra>",
            ),
            row=2,
            col=1,
        )

    # Row 3: resistances
    r_signals = [rll_uv, rll_uw, rll_vw, r_phase_avg]
    r_colors = cfg.palette[4:8]
    r_names = ["Rll UV", "Rll UW", "Rll VW", "R phase avg"]
    for s, name, color in zip(r_signals, r_names, r_colors):
        if s:
            fig.add_trace(
                go.Scatter(
                    x=t,
                    y=df[s],
                    mode="lines",
                    name=name,
                    line=dict(color=color, width=1.5),
                    hovertemplate="%{y:.4f} mΩ<extra>" + name + "</extra>",
                ),
                row=3,
                col=1,
            )

    fig.update_yaxes(title_text="A", row=1, col=1)
    fig.update_yaxes(title_text="V", row=2, col=1)
    fig.update_yaxes(title_text="mΩ", row=3, col=1)
    fig.update_xaxes(title_text="Time (s)", row=3, col=1)

    # Title / layout
    title_text = f"<b>{TITLE}</b><br><sup>{meta.title_line()}</sup>"
    core.apply_base_layout(
        fig,
        meta=meta,
        cfg=cfg,
        title=TITLE,
        subtitle=meta.source.name,
    )
    fig.update_layout(
        height=cfg.height_px_per_row * 3,
        width=cfg.width_px,
        legend=dict(orientation="h", yanchor="bottom", y=-0.18, xanchor="center", x=0.5),
        margin=dict(l=70, r=40, t=90, b=110),
    )
    return fig
