#!/usr/bin/env python3
"""
rteplot.py — CLI for turning RTE .jsonl telemetry logs into publication-ready
Plotly figures.

Examples:
    # Show what is inside a log
    ./rteplot.py info overmodulation_test.jsonl

    # Render a built-in recipe
    ./rteplot.py plot overmodulation_test.jsonl --recipe overmodulation -o overmod.png

    # Same, but as an interactive HTML file
    ./rteplot.py plot overmodulation_test.jsonl --recipe overmodulation -o overmod.html

    # Focus on a specific time window and fewer points
    ./rteplot.py plot fw-problem.jsonl --recipe fieldweakening \
        -o fw.png --t0 0.5 --t1 2.0 --max-points 2000

    # List available recipes
    ./rteplot.py recipes
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import plotly.io as pio

import rteplot_lib as core
from rteplot_lib import PlotConfig


def cmd_info(args: argparse.Namespace) -> int:
    meta, df = core.parse_jsonl(Path(args.log))
    print(f"source:      {meta.source}")
    print(f"started:     {meta.started_at}")
    print(f"exported:    {meta.exported_at}")
    print(f"mode:        {meta.mode}")
    print(f"port:        {meta.port}")
    print(f"protocol:    {meta.protocol}")
    print(f"telemetry:   {meta.telemetry_count:,} rows")
    print(f"duration:    {meta.duration_s:.3f} s")
    print(f"t range:     {df['t'].min():.4f} .. {df['t'].max():.4f}")
    print("signals:")
    for s in meta.signals:
        print(f"  - {s}")
    return 0


def cmd_recipes(args: argparse.Namespace) -> int:
    print("available recipes:")
    for name in core.list_recipes():
        try:
            mod = __import__(f"recipes.{name}", fromlist=["TITLE", "DESCRIPTION"])
            title = getattr(mod, "TITLE", name)
            desc = getattr(mod, "DESCRIPTION", "")
            print(f"  {name:20s}  {title}")
            if desc:
                print(f"    {desc}")
        except Exception as exc:
            print(f"  {name:20s}  (error loading: {exc})")
    return 0


def cmd_anomalies(args: argparse.Namespace) -> int:
    meta, df = core.parse_jsonl(Path(args.log))
    if df.empty:
        print(f"error: no telemetry found in {args.log}", file=sys.stderr)
        return 1

    summary = core.summarize_anomalies(df)
    print(f"source:   {meta.source}")
    print(f"duration: {summary['duration_s']:.3f} s, samples: {summary['samples']:,}")
    if "rpm_max" in summary:
        print(f"rpm:      {summary['rpm_min']:.1f} .. {summary['rpm_max']:.1f}")
    if "m_idx_max" in summary:
        print(f"m_idx:    max {summary['m_idx_max']:.3f}, >1 for {summary['m_idx_over_1_pct']:.1f}% of run")
    if "id_std" in summary:
        print(f"id:       {summary['id_min']:.2f} .. {summary['id_max']:.2f} A (std {summary['id_std']:.2f} A)")
    if "iq_std" in summary:
        print(f"iq:       {summary['iq_min']:.2f} .. {summary['iq_max']:.2f} A (std {summary['iq_std']:.2f} A)")

    anomalies = core.find_anomalies(df)
    print(f"\ndetected anomalies: {len(anomalies)}")
    for a in anomalies:
        print(f"  {a.kind:20s}  {a.t0:8.3f}s .. {a.t1:8.3f}s  severity={a.severity:.3f}  {a.detail}")

    if args.plot:
        # Auto-render the worst anomaly window with fwanalysis recipe.
        if anomalies:
            worst = max(anomalies, key=lambda x: x.severity)
            pad = (worst.t1 - worst.t0) * 0.3 + 1.0
            t0 = max(0.0, worst.t0 - pad)
            t1 = worst.t1 + pad
            out = Path(args.plot)
            cfg = PlotConfig(time_window=(t0, t1), max_points=5000)
            plot_df = core.apply_window(df, cfg)
            plot_df = core.downsample(plot_df, cfg.max_points)
            fig = core.load_recipe(args.recipe)(plot_df, meta, cfg)
            if out.suffix.lower() == ".html":
                fig.write_html(str(out), include_plotlyjs="cdn")
            else:
                fig.write_image(str(out), scale=2.0)
            print(f"\nwrote anomaly plot: {out} (t={t0:.2f} .. {t1:.2f}s)")
    return 0


def cmd_plot(args: argparse.Namespace) -> int:
    log_path = Path(args.log)
    out_path = Path(args.output) if args.output else log_path.with_suffix(".png")

    cfg = PlotConfig(
        max_points=args.max_points,
        time_window=(args.t0, args.t1),
        width_px=args.width,
        height_px_per_row=args.height_per_row,
        logo_scale=args.logo_scale,
        show_legend=not args.no_legend,
    )
    if args.logo:
        cfg.logo_path = Path(args.logo)
    else:
        default_logo = Path(__file__).parent / "assets" / "logo.png"
        if default_logo.exists():
            cfg.logo_path = default_logo

    meta, df = core.parse_jsonl(log_path)
    if df.empty:
        print(f"error: no telemetry found in {log_path}", file=sys.stderr)
        return 1

    # Auto-window: if no explicit window given, zoom to the worst anomaly.
    if args.auto_window and cfg.time_window == (None, None):
        anomalies = core.find_anomalies(df)
        if anomalies:
            worst = max(anomalies, key=lambda a: a.severity)
            pad = (worst.t1 - worst.t0) * 0.4 + 0.5
            cfg.time_window = (
                max(0.0, worst.t0 - pad),
                worst.t1 + pad,
            )
            print(f"auto-window: {cfg.time_window[0]:.3f}s .. {cfg.time_window[1]:.3f}s ({worst.kind})")

    df = core.apply_window(df, cfg)
    if df.empty:
        print("error: selected time window contains no data", file=sys.stderr)
        return 1

    # Keep full interactive data, downsample only for static raster export.
    if out_path.suffix.lower() in {".png", ".jpg", ".jpeg", ".pdf", ".svg", ".webp"}:
        plot_df = core.downsample(df, cfg.max_points)
    else:
        plot_df = df

    make_fig = core.load_recipe(args.recipe)
    fig = make_fig(plot_df, meta, cfg)

    if out_path.suffix.lower() == ".html":
        fig.write_html(str(out_path), include_plotlyjs="cdn")
    else:
        fig.write_image(str(out_path), scale=args.scale)

    print(f"wrote {out_path} ({len(plot_df):,} plotted points)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="rteplot",
        description="Generate polished figures from RTE .jsonl logs.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    info = sub.add_parser("info", help="summarise a log file")
    info.add_argument("log", help="path to .jsonl log")

    recipes = sub.add_parser("recipes", help="list built-in recipes")

    plot = sub.add_parser("plot", help="render a figure from a log")
    plot.add_argument("log", help="path to .jsonl log")
    plot.add_argument(
        "--recipe", "-r", required=True,
        help="recipe name (see 'rteplot recipes')",
    )
    plot.add_argument(
        "--output", "-o", default=None,
        help="output file (html/png/pdf/svg/jpg). default: <log>.png",
    )
    plot.add_argument(
        "--t0", type=float, default=None,
        help="start time (s) to crop",
    )
    plot.add_argument(
        "--t1", type=float, default=None,
        help="end time (s) to crop",
    )
    plot.add_argument(
        "--max-points", type=int, default=5_000,
        help="downsample raster outputs to this many points (default 5000)",
    )
    plot.add_argument(
        "--width", type=int, default=1280,
        help="figure width in pixels (default 1280)",
    )
    plot.add_argument(
        "--height-per-row", type=int, default=260,
        help="subplot height per row in pixels (default 260)",
    )
    plot.add_argument(
        "--scale", type=float, default=2.0,
        help="raster export scale factor, e.g. 2 for retina (default 2)",
    )
    plot.add_argument(
        "--logo", type=str, default=None,
        help="path to logo image (png/svg/jpg). default: assets/logo.png",
    )
    plot.add_argument(
        "--logo-scale", type=float, default=0.18,
        help="logo size as fraction of figure width (default 0.18)",
    )
    plot.add_argument(
        "--auto-window", action="store_true",
        help="auto-crop to the worst anomaly (ignores --t0/--t1)",
    )
    plot.add_argument(
        "--no-legend", action="store_true",
        help="hide the legend",
    )

    anomalies_cmd = sub.add_parser("anomalies", help="detect and report anomalies")
    anomalies_cmd.add_argument("log", help="path to .jsonl log")
    anomalies_cmd.add_argument(
        "--plot", "-p", default=None,
        help="also render the worst anomaly to this file",
    )
    anomalies_cmd.add_argument(
        "--recipe", "-r", default="fwanalysis",
        help="recipe used for the anomaly plot (default: fwanalysis)",
    )

    args = parser.parse_args(argv)

    if args.command == "info":
        return cmd_info(args)
    if args.command == "recipes":
        return cmd_recipes(args)
    if args.command == "plot":
        return cmd_plot(args)
    if args.command == "anomalies":
        return cmd_anomalies(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
