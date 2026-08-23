"""
Field-weakening diagnostic recipe.

High-speed operation requires negative id current to weaken the rotor flux.
This view stacks speed, currents, voltages, and modulation index so it is easy
to see whether the field-weakening trajectory is behaving or doing something
unexpected.
"""
from __future__ import annotations

import plotly.graph_objects as go

import rteplot_lib as core
from rteplot_lib import PlotConfig, SessionMeta

TITLE = "Field-Weakening Diagnostic"
DESCRIPTION = (
    "Speed, d/q currents, voltage vector, and modulation index. Look for id "
    "going the wrong way, voltage saturation, or iq collapsing at high speed."
)


def make_fig(df, meta: SessionMeta, cfg: PlotConfig):
    df = core.add_modulation_index(df)

    rpm = core.pick_signal(df, "Mech_RPM", "Elec_RPM", "RPM")
    id_a = core.pick_signal(df, "cg_id_a", "mpcc_id_a")
    iq_a = core.pick_signal(df, "cg_iq_a", "mpcc_iq_a")
    vd_v = core.pick_signal(df, "cg_vd_v", "mpcc_vd_v")
    vq_v = core.pick_signal(df, "cg_vq_v", "mpcc_vq_v")
    vdc = core.pick_signal(df, "vdc_v", "cg_vdc_v", "mpcc_vdc_v")

    # Speed may be in rad/s (mpcc_omega_e); try to convert to RPM-ish.
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

    # Shade overmodulation region
    if "m_idx" in df.columns and not df["m_idx"].empty:
        m_max = df["m_idx"].max()
        if m_max > 1.0:
            fig.add_hrect(
                y0=1.0,
                y1=m_max * 1.05,
                row=signals.index("m_idx") + 1,
                col=1,
                fillcolor="#d64550",
                opacity=0.08,
                line_width=0,
            )

    # Axis units
    for s, unit in [(id_a, "A"), (iq_a, "A"), (vd_v, "V"), (vq_v, "V"), (vdc, "V")]:
        if s and s in signals:
            fig.update_yaxes(title_text=unit, row=signals.index(s) + 1, col=1)

    if rpm and rpm in signals:
        fig.update_yaxes(title_text="RPM", row=signals.index(rpm) + 1, col=1)

    return fig
