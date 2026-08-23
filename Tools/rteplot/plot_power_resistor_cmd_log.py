"""
Parse Power-resistor-log-cmds.txt and render a V-I figure for the resistance
fit on the ~20 Ω power resistor.
"""
from __future__ import annotations

import re
from pathlib import Path

import numpy as np
import plotly.graph_objects as go
from plotly.subplots import make_subplots

LOG_PATH = Path("/home/tliao/Desktop/Power-resistor-log-cmds.txt")
OUT_PNG = Path("/home/tliao/Desktop/power-resistor-vi-fit.png")
OUT_HTML = Path("/home/tliao/Desktop/power-resistor-vi-fit.html")


def parse_cmd_log(path: Path) -> dict:
    sections: dict[str, dict] = {}
    current_phase = None
    for line in path.read_text().splitlines():
        m = re.search(r"RES: (UV|UW|VW) fit data: (.*)", line)
        if m:
            phase, data_str = m.groups()
            points = []
            for pm in re.finditer(r"\(V=([\d.]+)V I=([-?\d.]+)A\)", data_str):
                points.append((float(pm.group(1)), float(pm.group(2))))
            sections.setdefault(phase, {})["points"] = points
            current_phase = phase
            continue

        m = re.search(r"RES: (UV|UW|VW): R_ll=([-\d.]+) mohm\s+R_phase=([-\d.]+) mohm", line)
        if m:
            phase, rll, rphase = m.groups()
            sec = sections.setdefault(phase, {})
            sec["rll_mohm"] = float(rll)
            sec["rphase_mohm"] = float(rphase)
            continue

        m = re.search(r"RES: FAIL: (.*)", line)
        if m and current_phase:
            sections.setdefault(current_phase, {})["fail"] = m.group(1)

    return sections


def linear_fit(v: np.ndarray, i: np.ndarray):
    """Return slope (R_ll in V/A), intercept (V_off), and R^2."""
    n = len(v)
    sum_v = float(v.sum())
    sum_i = float(i.sum())
    sum_vi = float((v * i).sum())
    sum_ii = float((i * i).sum())
    denom = n * sum_ii - sum_i * sum_i
    if denom == 0:
        return None, None, None
    slope = (n * sum_vi - sum_v * sum_i) / denom
    intercept = (sum_v - slope * sum_i) / n
    ss_res = float(((v - (slope * i + intercept)) ** 2).sum())
    ss_tot = float(((v - v.mean()) ** 2).sum())
    r2 = 1.0 - ss_res / ss_tot if ss_tot else 0.0
    return slope, intercept, r2


def main():
    sections = parse_cmd_log(LOG_PATH)

    fig = make_subplots(
        rows=1,
        cols=2,
        subplot_titles=("UV: V vs I with linear fit", "UW: raw points (open phase)"),
        horizontal_spacing=0.12,
    )

    palette = ["#003c6c", "#f29813", "#13a665", "#d64550"]

    # UV fit and plot
    if "UV" in sections and "points" in sections["UV"]:
        pts = sections["UV"]["points"]
        v_arr = np.array([p[0] for p in pts])
        i_arr = np.array([p[1] for p in pts])
        slope, intercept, r2 = linear_fit(v_arr, i_arr)
        rll = sections["UV"].get("rll_mohm", slope * 1000 if slope else None)

        fig.add_trace(
            go.Scatter(
                x=i_arr,
                y=v_arr,
                mode="markers",
                name="UV measured",
                marker=dict(color=palette[0], size=10),
                hovertemplate="I=%{x:.3f} A<br>V=%{y:.3f} V<extra>UV</extra>",
            ),
            row=1,
            col=1,
        )

        if slope is not None:
            i_fit = np.linspace(i_arr.min() - 0.02, i_arr.max() + 0.02, 100)
            v_fit = slope * i_fit + intercept
            fig.add_trace(
                go.Scatter(
                    x=i_fit,
                    y=v_fit,
                    mode="lines",
                    name=f"UV fit (R_ll={rll/1000:.3f} Ω)",
                    line=dict(color=palette[1], width=2, dash="dash"),
                    hovertemplate="I=%{x:.3f} A<br>V=%{y:.3f} V<extra>fit</extra>",
                ),
                row=1,
                col=1,
            )

        fig.add_annotation(
            x=0.05,
            y=0.95,
            xref="x domain",
            yref="y domain",
            text=(
                f"<b>UV result</b><br>"
                f"R_ll = {rll/1000:.3f} Ω ({rll:.1f} mΩ)<br>"
                f"R_phase = {sections['UV'].get('rphase_mohm', rll/2)/1000:.3f} Ω<br>"
                f"V_offset = {intercept:.3f} V<br>"
                f"R² = {r2:.4f}"
            ),
            showarrow=False,
            align="left",
            bgcolor="rgba(255,255,255,0.85)",
            bordercolor="#cccccc",
            borderwidth=1,
            row=1,
            col=1,
        )

    # UW raw points
    if "UW" in sections and "points" in sections["UW"]:
        pts = sections["UW"]["points"]
        i_arr = np.array([p[1] for p in pts])
        v_arr = np.array([p[0] for p in pts])
        rll = sections["UW"].get("rll_mohm")
        fail = sections["UW"].get("fail", "")

        fig.add_trace(
            go.Scatter(
                x=i_arr,
                y=v_arr,
                mode="markers",
                name="UW measured",
                marker=dict(color=palette[3], size=10, symbol="x"),
                hovertemplate="I=%{x:.3f} A<br>V=%{y:.3f} V<extra>UW</extra>",
            ),
            row=1,
            col=2,
        )

        text = f"<b>UW result</b><br>R_ll = {rll:.1f} mΩ" if rll is not None else "<b>UW result</b>"
        if fail:
            text += f"<br>FAIL: {fail}"
        fig.add_annotation(
            x=0.05,
            y=0.95,
            xref="x2 domain",
            yref="y2 domain",
            text=text,
            showarrow=False,
            align="left",
            bgcolor="rgba(255,255,255,0.85)",
            bordercolor="#cccccc",
            borderwidth=1,
            row=1,
            col=2,
        )

    fig.update_xaxes(title_text="Current (A)", row=1, col=1)
    fig.update_yaxes(title_text="Line-to-line voltage (V)", row=1, col=1)
    fig.update_xaxes(title_text="Current (A)", row=1, col=2)
    fig.update_yaxes(title_text="Line-to-line voltage (V)", row=1, col=2)

    fig.update_layout(
        title=dict(
            text="<b>Power-resistor resistance calibration</b><br>"
                 "<sup>~20 Ω resistor tied to one phase (UV pair)</sup>",
            x=0.0,
            xanchor="left",
        ),
        template="plotly_white",
        width=1400,
        height=650,
        legend=dict(orientation="h", yanchor="bottom", y=-0.22, xanchor="center", x=0.5),
        margin=dict(l=70, r=50, t=90, b=90),
        hovermode="x unified",
    )

    fig.write_image(str(OUT_PNG), scale=2)
    fig.write_html(str(OUT_HTML), include_plotlyjs="cdn")
    print(f"wrote {OUT_PNG}")
    print(f"wrote {OUT_HTML}")


if __name__ == "__main__":
    main()
