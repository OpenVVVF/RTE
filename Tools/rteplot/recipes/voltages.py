"""
Generic three-phase and DQ voltage view.
"""
from __future__ import annotations

import rteplot_lib as core
from rteplot_lib import PlotConfig, SessionMeta

TITLE = "Voltages"
DESCRIPTION = "Phase voltages, d/q voltages, and DC link."


def make_fig(df, meta: SessionMeta, cfg: PlotConfig):
    vu = core.pick_signal(df, "cg_vu_v", "mpcc_vu_v")
    vv = core.pick_signal(df, "cg_vv_v", "mpcc_vv_v")
    vw = core.pick_signal(df, "cg_vw_v", "mpcc_vw_v")
    vd = core.pick_signal(df, "cg_vd_v", "mpcc_vd_v")
    vq = core.pick_signal(df, "cg_vq_v", "mpcc_vq_v")
    vdc = core.pick_signal(df, "vdc_v", "cg_vdc_v", "mpcc_vdc_v")

    signals = [s for s in [vu, vv, vw, vd, vq, vdc] if s]
    fig = core.make_subplot_fig(
        df=df,
        signals=signals,
        meta=meta,
        cfg=cfg,
        title=TITLE,
        subtitle=meta.source.name,
    )
    for s in signals:
        fig.update_yaxes(title_text="V", row=signals.index(s) + 1, col=1)
    return fig
