"""
Generic three-phase and DQ current view.
"""
from __future__ import annotations

import rteplot_lib as core
from rteplot_lib import PlotConfig, SessionMeta

TITLE = "Currents"
DESCRIPTION = "Phase currents and d/q axis currents."


def make_fig(df, meta: SessionMeta, cfg: PlotConfig):
    iu = core.pick_signal(df, "cg_iu_a", "mpcc_iu_a")
    iv = core.pick_signal(df, "cg_iv_a", "mpcc_iv_a")
    iw = core.pick_signal(df, "cg_iw_a", "mpcc_iw_a")
    id_a = core.pick_signal(df, "cg_id_a", "mpcc_id_a")
    iq_a = core.pick_signal(df, "cg_iq_a", "mpcc_iq_a")

    signals = [s for s in [iu, iv, iw, id_a, iq_a] if s]
    fig = core.make_subplot_fig(
        df=df,
        signals=signals,
        meta=meta,
        cfg=cfg,
        title=TITLE,
        subtitle=meta.source.name,
    )
    for s in signals:
        fig.update_yaxes(title_text="A", row=signals.index(s) + 1, col=1)
    return fig
