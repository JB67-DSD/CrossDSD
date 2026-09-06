#!/usr/bin/env python3
"""Measure AudioGate DSD derivatives against their DXD reference masters."""

from __future__ import annotations

import argparse
import csv
import json
import math
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import numpy as np

from audio_alignment import align_fractional, fractional_lag


ANALYSIS_RATE = 352_800
EXPECTED_DSD_GAIN_DB = -6.0206
GAIN_TOLERANCE_DB = 0.25
PHASE_TOLERANCE_DEG = 1.0


def db(value: float) -> float | None:
    if not math.isfinite(value) or value <= 0:
        return None
    return 20.0 * math.log10(value)


def rounded(value: float | None, digits: int = 6) -> float | None:
    return None if value is None or not math.isfinite(value) else round(value, digits)


def decode(path: Path) -> np.ndarray:
    process = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(path),
            "-map",
            "0:a:0",
            "-ar",
            str(ANALYSIS_RATE),
            "-ac",
            "2",
            "-f",
            "f64le",
            "-acodec",
            "pcm_f64le",
            "pipe:1",
        ],
        capture_output=True,
        check=True,
    )
    samples = np.frombuffer(process.stdout, dtype="<f8")
    if samples.size % 2:
        raise ValueError(f"Odd interleaved sample count decoded from {path}")
    return samples.reshape(-1, 2)


def signal_metrics(
    master: np.ndarray,
    derivative: np.ndarray,
    level_master: np.ndarray | None = None,
    level_derivative: np.ndarray | None = None,
) -> dict[str, Any]:
    m = master.reshape(-1)
    d = derivative.reshape(-1)
    lm = master if level_master is None else level_master
    ld = derivative if level_derivative is None else level_derivative
    master_rms = float(np.sqrt(np.mean(lm * lm)))
    derivative_rms = float(np.sqrt(np.mean(ld * ld)))
    master_peak = float(np.max(np.abs(lm)))
    derivative_peak = float(np.max(np.abs(ld)))
    master_energy = float(np.dot(m, m))
    derivative_energy = float(np.dot(d, d))
    gain = float(np.dot(m, d) / master_energy) if master_energy > 1e-24 else None
    correlation = (
        float(np.dot(m, d) / math.sqrt(master_energy * derivative_energy))
        if master_energy > 1e-24 and derivative_energy > 1e-24
        else None
    )
    return {
        "master_rms_dbfs": rounded(db(master_rms)),
        "derivative_rms_dbfs": rounded(db(derivative_rms)),
        "master_peak_dbfs": rounded(db(master_peak)),
        "derivative_peak_dbfs": rounded(db(derivative_peak)),
        "estimated_gain_db": rounded(db(abs(gain))) if gain is not None else None,
        "correlation": rounded(correlation, 9),
        "finite": bool(np.isfinite(derivative).all()),
    }


def complex_tone_amplitude(samples: np.ndarray, frequency: float, channel: int) -> complex:
    trim = min(int(0.1 * ANALYSIS_RATE), len(samples) // 10)
    x = samples[trim : len(samples) - trim, channel]
    n = np.arange(len(x), dtype=np.float64)
    oscillator = np.exp(-2j * np.pi * frequency * n / ANALYSIS_RATE)
    return complex(np.dot(x, oscillator))


def wrap_degrees(value: float) -> float:
    return (value + 180.0) % 360.0 - 180.0


def phase_relation(samples: np.ndarray, frequency: float = 1000.0) -> float:
    left = complex_tone_amplitude(samples, frequency, 0)
    right = complex_tone_amplitude(samples, frequency, 1)
    return math.degrees(np.angle(right / left))


def impulse_channel_check(samples: np.ndarray) -> dict[str, Any]:
    left_peak = int(np.argmax(np.abs(samples[:, 0])))
    right_peak = int(np.argmax(np.abs(samples[:, 1])))
    window = 512

    def separation_db(position: int, wanted: int, unwanted: int) -> float | None:
        lo, hi = max(0, position - window), min(len(samples), position + window + 1)
        wanted_peak = float(np.max(np.abs(samples[lo:hi, wanted])))
        unwanted_peak = float(np.max(np.abs(samples[lo:hi, unwanted])))
        if unwanted_peak == 0:
            return None
        return rounded(20 * math.log10(wanted_peak / unwanted_peak), 3)

    return {
        "left_peak_sample": left_peak,
        "right_peak_sample": right_peak,
        "left_before_right": left_peak < right_peak,
        "left_event_separation_db": separation_db(left_peak, 0, 1),
        "right_event_separation_db": separation_db(right_peak, 1, 0),
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fields = [
        "test_id",
        "file_type",
        "file",
        "source_file",
        "master_samples",
        "decoded_samples",
        "decoded_duration_s",
        "latency_samples",
        "latency_us",
        "trailing_padding_samples",
        "trailing_padding_ms",
        "master_rms_dbfs",
        "derivative_rms_dbfs",
        "master_peak_dbfs",
        "derivative_peak_dbfs",
        "estimated_gain_db",
        "correlation",
        "finite",
    ]
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows({field: row.get(field, "") for field in fields} for row in rows)


def fmt(value: Any, digits: int = 3) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def write_markdown(path: Path, report: dict[str, Any]) -> None:
    lines = [
        "# CrossDSD Audio Validation Report",
        "",
        f"- Status: **{report['status'].upper()}**",
        f"- Analysis rate: {ANALYSIS_RATE:,} Hz",
        f"- Files analyzed: {report['files_analyzed']} derivatives and 31 masters",
        f"- Decoder: `{report['decoder']}`",
        "",
        "## Method",
        "",
        "Every DSF was decoded by FFmpeg to stereo float64 PCM at 352.8 kHz. The peak of",
        "`A002_single_impulse_-6dBFS` establishes the alignment latency independently for",
        "DSD64, DSD128 and DSD256. Measurements use the aligned interval corresponding to",
        "the DXD master. RMS and peak values are unweighted full-band measurements.",
        "",
        "The expected steady-state DSD convention gain is -6.0206 dB. Compliance is checked",
        f"on the -20 dBFS and -6 dBFS 1 kHz tones with a ±{GAIN_TOLERANCE_DB} dB tolerance.",
        "",
        "## Latency and padding",
        "",
        "| Type | Latency (samples) | Latency (µs) | Trailing padding min/median/max (ms) |",
        "|---|---:|---:|---:|",
    ]
    for file_type, item in report["summary"]["timing"].items():
        lines.append(
            f"| {file_type} | {item['latency_samples']} | {fmt(item['latency_us'])} | "
            f"{fmt(item['trailing_padding_ms_min'])} / "
            f"{fmt(item['trailing_padding_ms_median'])} / "
            f"{fmt(item['trailing_padding_ms_max'])} |"
        )

    lines += [
        "",
        "## 1 kHz level and gain",
        "",
        "| Signal | Type | Master RMS | Derivative RMS | Estimated gain | Result |",
        "|---|---|---:|---:|---:|---|",
    ]
    for item in report["summary"]["tone_gain_checks"]:
        lines.append(
            f"| {item['test_id']} | {item['file_type']} | "
            f"{fmt(item['master_rms_dbfs'])} dBFS | "
            f"{fmt(item['derivative_rms_dbfs'])} dBFS | "
            f"{fmt(item['estimated_gain_db'])} dB | {item['status']} |"
        )

    lines += [
        "",
        "## Digital silence",
        "",
        "| Type | Decoded RMS | Decoded peak |",
        "|---|---:|---:|",
    ]
    for item in report["summary"]["silence"]:
        lines.append(
            f"| {item['file_type']} | {fmt(item['derivative_rms_dbfs'])} dBFS | "
            f"{fmt(item['derivative_peak_dbfs'])} dBFS |"
        )

    lines += [
        "",
        "## Channel order and separation",
        "",
        "| Type | L before R | L-event separation | R-event separation | Result |",
        "|---|---|---:|---:|---|",
    ]
    for item in report["summary"]["channel_checks"]:
        lines.append(
            f"| {item['file_type']} | {item['left_before_right']} | "
            f"{fmt(item['left_event_separation_db'])} dB | "
            f"{fmt(item['right_event_separation_db'])} dB | {item['status']} |"
        )

    lines += [
        "",
        "## Interchannel phase",
        "",
        "| Signal | Type | Master phase | Derivative phase | Error | Result |",
        "|---|---|---:|---:|---:|---|",
    ]
    for item in report["summary"]["phase_checks"]:
        lines.append(
            f"| {item['test_id']} | {item['file_type']} | "
            f"{fmt(item['master_phase_deg'])}° | {fmt(item['derivative_phase_deg'])}° | "
            f"{fmt(item['phase_error_deg'])}° | {item['status']} |"
        )

    lines += [
        "",
        "## Stability and full-scale checks",
        "",
        "| Signal | Type | Peak | Finite | Clipping detected | Result |",
        "|---|---|---:|---|---|---|",
    ]
    for item in report["summary"]["stability_checks"]:
        lines.append(
            f"| {item['test_id']} | {item['file_type']} | "
            f"{fmt(item['derivative_peak_dbfs'])} dBFS | {item['finite']} | "
            f"{item['clipping_detected']} | {item['status']} |"
        )

    lines += ["", "## Findings", ""]
    if report["findings"]:
        for finding in report["findings"]:
            lines.append(
                f"- **{finding['severity'].upper()} — {finding['code']}**: "
                f"{finding['message']}"
            )
    else:
        lines.append("- No validation error or warning.")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Benchmark suite directory.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Report directory (default: <root>/reports/audio-validation).",
    )
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    output = (
        args.output_dir.expanduser().resolve()
        if args.output_dir
        else root / "reports" / "audio-validation"
    )

    if shutil.which("ffmpeg") is None:
        print("ERROR: ffmpeg is required.", file=sys.stderr)
        return 2
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("ERROR: numpy is required.", file=sys.stderr)
        return 2

    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    masters = {row["test_id"]: row for row in manifest if row["role"] == "reference_master"}
    derivatives = [row for row in manifest if row["role"] == "dsd_derivative"]
    by_type: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in derivatives:
        by_type[row["file_type"]].append(row)

    decoder_line = subprocess.run(
        ["ffmpeg", "-version"], capture_output=True, text=True, check=True
    ).stdout.splitlines()[0]

    latencies: dict[str, float] = {}
    impulse_master = decode(root / masters["A002"]["file"])
    for file_type, rows in sorted(by_type.items()):
        impulse_row = next(row for row in rows if row["test_id"] == "A002")
        impulse_dsd = decode(root / impulse_row["file"])
        latencies[file_type] = fractional_lag(impulse_master, impulse_dsd)

    results: list[dict[str, Any]] = []
    arrays: dict[tuple[str, str], np.ndarray] = {}
    master_arrays: dict[str, np.ndarray] = {}
    for index, row in enumerate(derivatives, 1):
        test_id = row["test_id"]
        if test_id not in master_arrays:
            master_arrays[test_id] = decode(root / masters[test_id]["file"])
        master = master_arrays[test_id]
        derivative = decode(root / row["file"])
        lag = latencies[row["file_type"]]
        m, d = align_fractional(master, derivative, lag)
        lm, ld = align_fractional(master, derivative, float(math.floor(lag)))
        metrics = signal_metrics(m, d, lm, ld)
        trailing = len(derivative) - lag - len(master)
        result = {
            "test_id": test_id,
            "file_type": row["file_type"],
            "file": row["file"],
            "source_file": row["source_file"],
            "master_samples": len(master),
            "decoded_samples": len(derivative),
            "decoded_duration_s": rounded(len(derivative) / ANALYSIS_RATE),
            "latency_samples": rounded(lag, 6),
            "latency_us": rounded(lag * 1_000_000 / ANALYSIS_RATE, 3),
            "trailing_padding_samples": trailing,
            "trailing_padding_ms": rounded(trailing * 1000 / ANALYSIS_RATE, 3),
            **metrics,
        }
        results.append(result)
        if test_id in {"A003", "B013", "B014"}:
            arrays[(test_id, row["file_type"])] = d.copy()
        print(f"[{index:02d}/{len(derivatives)}] {row['file_type']} {test_id}", flush=True)

    findings: list[dict[str, str]] = []

    def finding(severity: str, code: str, message: str) -> None:
        findings.append({"severity": severity, "code": code, "message": message})

    timing: dict[str, Any] = {}
    for file_type in sorted(by_type):
        type_rows = [r for r in results if r["file_type"] == file_type]
        padding = np.array([r["trailing_padding_ms"] for r in type_rows], dtype=float)
        timing[file_type] = {
            "latency_samples": rounded(latencies[file_type], 6),
            "latency_us": rounded(latencies[file_type] * 1_000_000 / ANALYSIS_RATE, 3),
            "trailing_padding_ms_min": rounded(float(np.min(padding)), 3),
            "trailing_padding_ms_median": rounded(float(np.median(padding)), 3),
            "trailing_padding_ms_max": rounded(float(np.max(padding)), 3),
        }
        if np.min(padding) < 0:
            finding("error", "negative_padding", f"{file_type} has truncated decoded content.")

    tone_checks: list[dict[str, Any]] = []
    for test_id in ("B004", "B005"):
        for row in [r for r in results if r["test_id"] == test_id]:
            deviation = abs(row["estimated_gain_db"] - EXPECTED_DSD_GAIN_DB)
            status = "PASS" if deviation <= GAIN_TOLERANCE_DB else "FAIL"
            tone_checks.append({**row, "gain_deviation_db": rounded(deviation), "status": status})
            if status == "FAIL":
                finding(
                    "error",
                    "gain_out_of_tolerance",
                    f"{row['file_type']} {test_id}: gain {row['estimated_gain_db']} dB.",
                )

    silence = [r for r in results if r["test_id"] == "A001"]

    channel_checks: list[dict[str, Any]] = []
    for file_type in sorted(by_type):
        check = impulse_channel_check(arrays[("A003", file_type)])
        check.update({"file_type": file_type, "status": "PASS" if check["left_before_right"] else "FAIL"})
        channel_checks.append(check)
        if not check["left_before_right"]:
            finding("error", "channel_order", f"{file_type}: left/right impulse order is reversed.")

    phase_checks: list[dict[str, Any]] = []
    for test_id in ("B013", "B014"):
        master_phase = phase_relation(master_arrays[test_id])
        for file_type in sorted(by_type):
            derivative_phase = phase_relation(arrays[(test_id, file_type)])
            error = wrap_degrees(derivative_phase - master_phase)
            status = "PASS" if abs(error) <= PHASE_TOLERANCE_DEG else "FAIL"
            phase_checks.append(
                {
                    "test_id": test_id,
                    "file_type": file_type,
                    "master_phase_deg": rounded(master_phase, 4),
                    "derivative_phase_deg": rounded(derivative_phase, 4),
                    "phase_error_deg": rounded(error, 4),
                    "status": status,
                }
            )
            if status == "FAIL":
                finding(
                    "error",
                    "phase_out_of_tolerance",
                    f"{file_type} {test_id}: interchannel phase error {error:.4f}°.",
                )

    stability_checks: list[dict[str, Any]] = []
    for test_id in ("F001", "F004"):
        for row in [r for r in results if r["test_id"] == test_id]:
            peak_linear = 10 ** (row["derivative_peak_dbfs"] / 20)
            clipping = peak_linear >= 0.999999
            status = "PASS" if row["finite"] and not clipping else "FAIL"
            stability_checks.append(
                {
                    "test_id": test_id,
                    "file_type": row["file_type"],
                    "derivative_peak_dbfs": row["derivative_peak_dbfs"],
                    "finite": row["finite"],
                    "clipping_detected": clipping,
                    "status": status,
                }
            )
            if status == "FAIL":
                finding(
                    "error",
                    "stability_or_clipping",
                    f"{row['file_type']} {test_id}: non-finite value or clipping.",
                )

    status = "pass" if not any(f["severity"] == "error" for f in findings) else "fail"
    report = {
        "status": status,
        "analysis_rate_hz": ANALYSIS_RATE,
        "files_analyzed": len(results),
        "decoder": decoder_line,
        "thresholds": {
            "expected_dsd_gain_db": EXPECTED_DSD_GAIN_DB,
            "gain_tolerance_db": GAIN_TOLERANCE_DB,
            "phase_tolerance_deg": PHASE_TOLERANCE_DEG,
        },
        "summary": {
            "timing": timing,
            "tone_gain_checks": tone_checks,
            "silence": silence,
            "channel_checks": channel_checks,
            "phase_checks": phase_checks,
            "stability_checks": stability_checks,
        },
        "findings": findings,
        "measurements": results,
    }

    output.mkdir(parents=True, exist_ok=True)
    (output / "audio-validation.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    write_csv(output / "audio-validation.csv", results)
    write_markdown(output / "audio-validation.md", report)
    print(
        f"{status.upper()}: {len(results)} derivatives analyzed; "
        f"{len(findings)} findings. Reports: {output}"
    )
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
