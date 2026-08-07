"""
Overmodulation / voltage-saturation recipe.

Shows the voltage-reference magnitude against the available DC-link headroom.
The key diagnostic is the modulation index ``m_idx`` = |Vdq| / (Vdc/√3).
Values above 1 mean the controller is asking for more voltage than the inverter
can produce — overmodulation / saturation.
"""
from __future__ import annotations

import numpy as np
import plotly.graph_objects as go

import rteplot_lib as core
from rteplot_lib import PlotConfig, SessionMeta

TITLE = "Overmodulation / Voltage Saturation"
DESCRIPTION = (
    "Voltage vector magnitude and modulation index. m_idx > 1 indicates the "
    "inverter is commanded beyond its linear modulation limit."
)


def make_fig(df, meta: SessionMeta, cfg: PlotConfig):
    df = core.add_modulation_index(df)

    vd = core.pick_signal(df, "cg_vd_v", "mpcc_vd_v")
    vq = core.pick_signal(df, "cg_vq_v", "mpcc_vq_v")
    vdc = core.pick_signal(df, "vdc_v", "cg_vdc_v", "mpcc_vdc_v")

    has_midx = "m_idx" in df.columns and df["m_idx"].notna().any()
    signals = [s for s in [vd, vq, vdc, "m_idx" if has_midx else None] if s]

    fig = core.make_subplot_fig(
        df=df,
        signals=signals,
        meta=meta,
        cfg=cfg,
        title=TITLE,
        subtitle=f"{meta.source.name} — saturation limit at m_idx = 1",
        hlines={"m_idx": 1.0} if "m_idx" in signals else None,
    )

    # Add a shaded warning region above m_idx = 1
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

    for s, unit in [(vd, "V"), (vq, "V"), (vdc, "V")]:
        if s and s in signals:
            fig.update_yaxes(title_text=unit, row=signals.index(s) + 1, col=1)

    if "m_idx" in signals:
        fig.update_yaxes(title_text="modulation index", row=signals.index("m_idx") + 1, col=1)

    return fig
