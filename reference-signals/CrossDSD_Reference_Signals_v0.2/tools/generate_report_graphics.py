#!/usr/bin/env python3
"""Generate deterministic SVG summaries for CrossDSD Benchmark Suite."""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path


COLORS = {
    ("AudioGate", "DSD64"): "#1769aa",
    ("AudioGate", "DSD128"): "#2e9d57",
    ("AudioGate", "DSD256"): "#d17a00",
    ("FFmpeg", "DSD64"): "#7d4ab5",
    ("FFmpeg", "DSD128"): "#c83d55",
    ("FFmpeg", "DSD256"): "#607d8b",
}


def esc(value: object) -> str:
    return (
        str(value)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def svg_chart(
    title: str,
    subtitle: str,
    series: list[tuple[str, str, list[tuple[float, float]]]],
    output: Path,
    x_label: str,
    y_label: str,
    *,
    log_x: bool = False,
) -> None:
    width, height = 1120, 680
    left, right, top, bottom = 92, 250, 86, 86
    plot_w, plot_h = width - left - right, height - top - bottom
    xs = [x for _, _, points in series for x, _ in points]
    ys = [y for _, _, points in series for _, y in points]
    if not xs or not ys:
        raise ValueError(f"No data for {title}")
    tx = (lambda x: math.log10(x)) if log_x else (lambda x: x)
    xmin, xmax = min(map(tx, xs)), max(map(tx, xs))
    ymin, ymax = min(ys), max(ys)
    ypad = max((ymax - ymin) * 0.12, 0.25)
    ymin, ymax = ymin - ypad, ymax + ypad

    def px(x: float) -> float:
        return left + (tx(x) - xmin) / (xmax - xmin or 1) * plot_w

    def py(y: float) -> float:
        return top + (ymax - y) / (ymax - ymin or 1) * plot_h

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        "<style>text{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;fill:#17202a}"
        ".title{font-size:25px;font-weight:650}.sub{font-size:14px;fill:#53606c}"
        ".axis{font-size:12px;fill:#53606c}.label{font-size:14px;font-weight:600}"
        ".legend{font-size:13px}.grid{stroke:#d9dee3;stroke-width:1}.frame{stroke:#75808a;fill:#fbfcfd}</style>",
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text class="title" x="{left}" y="38">{esc(title)}</text>',
        f'<text class="sub" x="{left}" y="62">{esc(subtitle)}</text>',
        f'<rect class="frame" x="{left}" y="{top}" width="{plot_w}" height="{plot_h}"/>',
    ]
    for i in range(6):
        y = ymin + i * (ymax - ymin) / 5
        yy = py(y)
        parts += [
            f'<line class="grid" x1="{left}" y1="{yy:.2f}" x2="{left + plot_w}" y2="{yy:.2f}"/>',
            f'<text class="axis" x="{left - 10}" y="{yy + 4:.2f}" text-anchor="end">{y:.1f}</text>',
        ]
    xticks = sorted(set(xs))
    for x in xticks:
        xx = px(x)
        label = f"{x / 1000:g}k" if x >= 1000 else f"{x:g}"
        parts += [
            f'<line class="grid" x1="{xx:.2f}" y1="{top}" x2="{xx:.2f}" y2="{top + plot_h}"/>',
            f'<text class="axis" x="{xx:.2f}" y="{top + plot_h + 22}" text-anchor="middle">{label}</text>',
        ]
    for decoder, rate, points in series:
        color = COLORS[(decoder, rate)]
        coords = " ".join(f"{px(x):.2f},{py(y):.2f}" for x, y in points)
        parts.append(
            f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="2.5" '
            'stroke-linejoin="round" stroke-linecap="round"/>'
        )
        for x, y in points:
            parts.append(f'<circle cx="{px(x):.2f}" cy="{py(y):.2f}" r="3" fill="{color}"/>')
    lx, ly = left + plot_w + 28, top + 22
    for idx, (decoder, rate, _) in enumerate(series):
        y = ly + idx * 28
        color = COLORS[(decoder, rate)]
        parts += [
            f'<line x1="{lx}" y1="{y}" x2="{lx + 30}" y2="{y}" stroke="{color}" stroke-width="3"/>',
            f'<text class="legend" x="{lx + 40}" y="{y + 5}">{esc(decoder)} · {esc(rate)}</text>',
        ]
    parts += [
        f'<text class="label" x="{left + plot_w / 2}" y="{height - 26}" text-anchor="middle">{esc(x_label)}</text>',
        f'<text class="label" transform="translate(24 {top + plot_h / 2}) rotate(-90)" text-anchor="middle">{esc(y_label)}</text>',
        "</svg>",
    ]
    output.write_text("\n".join(parts) + "\n", encoding="utf-8")


def bar_chart(
    title: str,
    subtitle: str,
    values: list[tuple[str, str, float]],
    output: Path,
) -> None:
    width, height = 980, 590
    left, right, top, bottom = 95, 45, 90, 115
    plot_w, plot_h = width - left - right, height - top - bottom
    ymax = max(v for _, _, v in values) * 1.18
    bar_w = plot_w / len(values) * 0.62
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        "<style>text{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;fill:#17202a}"
        ".title{font-size:25px;font-weight:650}.sub{font-size:14px;fill:#53606c}"
        ".axis{font-size:12px;fill:#53606c}.value{font-size:13px;font-weight:600}"
        ".grid{stroke:#d9dee3;stroke-width:1}.frame{stroke:#75808a;fill:#fbfcfd}</style>",
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text class="title" x="{left}" y="38">{esc(title)}</text>',
        f'<text class="sub" x="{left}" y="62">{esc(subtitle)}</text>',
        f'<rect class="frame" x="{left}" y="{top}" width="{plot_w}" height="{plot_h}"/>',
    ]
    for i in range(6):
        value = ymax * i / 5
        y = top + plot_h - value / ymax * plot_h
        parts += [
            f'<line class="grid" x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}"/>',
            f'<text class="axis" x="{left - 10}" y="{y + 4:.2f}" text-anchor="end">{value:.1f}</text>',
        ]
    slot = plot_w / len(values)
    for idx, (decoder, rate, value) in enumerate(values):
        x = left + idx * slot + (slot - bar_w) / 2
        h = value / ymax * plot_h
        y = top + plot_h - h
        color = COLORS[(decoder, rate)]
        parts += [
            f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_w:.2f}" height="{h:.2f}" fill="{color}" rx="3"/>',
            f'<text class="value" x="{x + bar_w / 2:.2f}" y="{y - 8:.2f}" text-anchor="middle">{value:.3f}</text>',
            f'<text class="axis" x="{x + bar_w / 2:.2f}" y="{top + plot_h + 24}" text-anchor="middle">{esc(rate)}</text>',
            f'<text class="axis" transform="translate({x + bar_w / 2:.2f} {top + plot_h + 42}) rotate(-35)" '
            f'text-anchor="end">{esc(decoder)}</text>',
        ]
    parts += [
        f'<text class="axis" transform="translate(24 {top + plot_h / 2}) rotate(-90)" text-anchor="middle">DXD samples</text>',
        "</svg>",
    ]
    output.write_text("\n".join(parts) + "\n", encoding="utf-8")


def get_rows(payload: object) -> list[dict]:
    if isinstance(payload, list):
        return payload
    if isinstance(payload, dict):
        for key in ("results", "measurements", "rows", "comparisons"):
            value = payload.get(key)
            if isinstance(value, list):
                return value
    raise ValueError("Unsupported report JSON structure")


def pick(row: dict, *names: str) -> object:
    for name in names:
        if name in row:
            return row[name]
    raise KeyError(f"None of {names!r} found in report row")


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    reports = root / "reports"
    output = reports / "graphics"
    output.mkdir(parents=True, exist_ok=True)
    decoder_payload = json.loads(
        (reports / "decoder-comparison" / "decoder-comparison.json").read_text(encoding="utf-8")
    )
    advanced_payload = json.loads(
        (reports / "advanced-metrics" / "advanced-metrics.json").read_text(encoding="utf-8")
    )
    decoder_rows = get_rows(decoder_payload)
    advanced_rows = get_rows(advanced_payload)

    grouped: dict[tuple[str, str], list[tuple[float, float]]] = {}
    tone_frequencies = {
        "B001": 20,
        "B002": 100,
        "B003": 440,
        "B004": 1000,
        "B005": 10000,
        "B006": 18000,
        "B007": 20000,
        "B008": 22000,
        "B009": 30000,
        "B010": 40000,
        "B011": 50000,
        "B012": 80000,
    }
    for row in decoder_rows:
        test_id = str(row["test_id"])
        if test_id not in tone_frequencies:
            continue
        rate = str(row["source_type"])
        for decoder, field in (
            ("AudioGate", "audiogate_tone_gain_db"),
            ("FFmpeg", "ffmpeg_tone_gain_db"),
        ):
            if row.get(field) is not None:
                grouped.setdefault((decoder, rate), []).append(
                    (tone_frequencies[test_id], float(row[field]))
                )
    tone_series = [(d, r, sorted(points)) for (d, r), points in sorted(grouped.items())]
    svg_chart(
        "Response to sinusoidal test signals",
        "Level change after a PCM → DSD → PCM round trip; 0 dB is ideal.",
        tone_series,
        output / "tone-response.svg",
        "Frequency (Hz)",
        "Level change (dB)",
        log_x=True,
    )

    b004: dict[tuple[str, str], list[tuple[float, float]]] = {}
    band_fields = (
        ("error_0_20k_dbfs", 10_000),
        ("error_20_30k_dbfs", 25_000),
        ("error_30_40k_dbfs", 35_000),
        ("error_40_50k_dbfs", 45_000),
        ("error_50_88k2_dbfs", 69_100),
        ("error_88k2_176k4_dbfs", 132_300),
    )
    for row in advanced_rows:
        if str(row["test_id"]) != "B004":
            continue
        decoder, rate = str(row["decoder"]), str(row["source_type"])
        b004[(decoder, rate)] = [(x, float(row[field])) for field, x in band_fields]
    if b004:
        svg_chart(
            "Error spectrum — B004 signal",
            "RMS error by frequency band after fractional alignment.",
            [(d, r, sorted(points)) for (d, r), points in sorted(b004.items())],
            output / "b004-error-bands.svg",
            "Representative band frequency (Hz)",
            "Error (dBFS)",
            log_x=True,
        )

    latencies: list[tuple[str, str, float]] = []
    seen: set[tuple[str, str]] = set()
    for row in advanced_payload["impulse_summary"]:
        key = (str(row["decoder"]), str(row["source_type"]))
        if key not in seen:
            latencies.append((*key, float(row["latency_samples"])))
            seen.add(key)
    bar_chart(
        "Measured fractional latency",
        "Alignment offset used to compare waveforms while accounting for sub-sample delay.",
        sorted(latencies),
        output / "fractional-latency.svg",
    )
    print(f"Generated {len(list(output.glob('*.svg')))} SVG graphics in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
