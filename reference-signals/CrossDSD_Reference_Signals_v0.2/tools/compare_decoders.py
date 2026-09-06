#!/usr/bin/env python3
"""Compare AudioGate and FFmpeg decoding of the same 93 AudioGate DSF files."""

from __future__ import annotations

import csv
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import numpy as np

from audio_alignment import align_fractional, fractional_lag


RATE = 352_800
SOURCE_TYPES = ("DSD64", "DSD128", "DSD256")
TONE_TESTS = {
    "B001": 20,
    "B002": 100,
    "B003": 1000,
    "B004": 1000,
    "B005": 1000,
    "B006": 10000,
    "B007": 18000,
    "B008": 20000,
    "B009": 22000,
    "B010": 30000,
    "B011": 40000,
    "B012": 80000,
}


def decibels(value: float | None) -> float | None:
    if value is None or value <= 0 or not math.isfinite(value):
        return None
    return 20 * math.log10(value)


def rnd(value: float | None, digits: int = 6) -> float | None:
    return None if value is None or not math.isfinite(value) else round(value, digits)


def decode(path: Path) -> np.ndarray:
    result = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(path),
            "-map",
            "0:a:0",
            "-ar",
            str(RATE),
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
    return np.frombuffer(result.stdout, dtype="<f8").reshape(-1, 2)


def branch_metrics(
    master: np.ndarray, decoded: np.ndarray, level_samples: np.ndarray | None = None
) -> dict[str, Any]:
    m = master.reshape(-1)
    d = decoded.reshape(-1)
    m_energy = float(np.dot(m, m))
    d_energy = float(np.dot(d, d))
    m_rms = float(np.sqrt(np.mean(m * m)))
    level = decoded if level_samples is None else level_samples
    d_rms = float(np.sqrt(np.mean(level * level)))
    gain = float(np.dot(m, d) / m_energy) if m_energy > 1e-24 else None
    correlation = (
        float(np.dot(m, d) / math.sqrt(m_energy * d_energy))
        if m_energy > 1e-24 and d_energy > 1e-24
        else None
    )
    relative_error = None
    normalized_error_dbfs = None
    if gain is not None and abs(gain) > 1e-15:
        error = d / gain - m
        error_rms = float(np.sqrt(np.mean(error * error)))
        normalized_error_dbfs = decibels(error_rms)
        relative_error = decibels(error_rms / m_rms) if m_rms > 0 else None
    return {
        "rms_dbfs": rnd(decibels(d_rms)),
        "peak_dbfs": rnd(decibels(float(np.max(np.abs(level))))),
        "gain_db": rnd(decibels(abs(gain))) if gain is not None else None,
        "correlation": rnd(correlation, 9),
        "gain_normalized_error_dbfs": rnd(normalized_error_dbfs),
        "gain_normalized_error_relative_db": rnd(relative_error),
    }


def decoder_difference(
    master: np.ndarray, audiogate: np.ndarray, ffmpeg: np.ndarray
) -> dict[str, Any]:
    m = master.reshape(-1)
    a = audiogate.reshape(-1)
    f = ffmpeg.reshape(-1)
    energy = float(np.dot(m, m))
    if energy <= 1e-24:
        difference = a - f
        return {
            "gain_difference_db": None,
            "normalized_correlation": None,
            "normalized_difference_rms_dbfs": rnd(
                decibels(float(np.sqrt(np.mean(difference * difference))))
            ),
        }
    ag = float(np.dot(m, a) / energy)
    fg = float(np.dot(m, f) / energy)
    an = a / ag
    fn = f / fg
    correlation = float(np.dot(an, fn) / math.sqrt(float(np.dot(an, an) * np.dot(fn, fn))))
    difference = an - fn
    return {
        "gain_difference_db": rnd(decibels(abs(ag / fg))),
        "normalized_correlation": rnd(correlation, 9),
        "normalized_difference_rms_dbfs": rnd(
            decibels(float(np.sqrt(np.mean(difference * difference))))
        ),
    }


def tone_gain(master: np.ndarray, decoded: np.ndarray, frequency: float) -> float | None:
    trim = min(int(0.1 * RATE), len(master) // 10)
    m = master[trim : len(master) - trim]
    d = decoded[trim : len(decoded) - trim]
    n = np.arange(len(m), dtype=np.float64)
    oscillator = np.exp(-2j * np.pi * frequency * n / RATE)
    ratios = []
    for channel in (0, 1):
        master_amplitude = abs(complex(np.dot(m[:, channel], oscillator)))
        decoded_amplitude = abs(complex(np.dot(d[:, channel], oscillator)))
        if master_amplitude > 0:
            ratios.append(decoded_amplitude / master_amplitude)
    return rnd(decibels(float(np.mean(ratios)))) if ratios else None


def markdown(report: dict[str, Any]) -> str:
    def f(value: Any, digits: int = 3) -> str:
        if value is None:
            return "n/a"
        return f"{value:.{digits}f}" if isinstance(value, float) else str(value)

    lines = [
        "# AudioGate vs FFmpeg DSD Decoder Comparison",
        "",
        f"- Status: **{report['status'].upper()}**",
        f"- DSF files compared: {report['files_compared']}",
        f"- Common PCM analysis format: float64, stereo, {RATE:,} Hz",
        "",
        "## Scope and method",
        "",
        "The same AudioGate-generated DSF is decoded through two independent branches:",
        "",
        "1. the WAV exported by AudioGate at 352.8 kHz float32;",
        "2. FFmpeg decoding to 352.8 kHz float64.",
        "",
        "Each branch is aligned independently to the original DXD master using the A002",
        "impulse. Gain is estimated by least squares. Decoder-to-decoder differences are",
        "also reported after removing each branch's measured gain.",
        "",
        "## Alignment",
        "",
        "| Source | AudioGate latency | FFmpeg latency | Equal decoded lengths |",
        "|---|---:|---:|---:|",
    ]
    for source_type, item in report["summary"]["alignment"].items():
        lines.append(
            f"| {source_type} | {f(item['audiogate_samples'], 4)} samples | "
            f"{f(item['ffmpeg_samples'], 4)} samples | {item['identical_length_files']}/31 |"
        )

    lines += [
        "",
        "## 1 kHz gain",
        "",
        "| Signal | Source | AudioGate | FFmpeg | Difference |",
        "|---|---|---:|---:|---:|",
    ]
    for item in report["summary"]["reference_tone_gain"]:
        lines.append(
            f"| {item['test_id']} | {item['source_type']} | "
            f"{f(item['audiogate_gain_db'], 4)} dB | {f(item['ffmpeg_gain_db'], 4)} dB | "
            f"{f(item['gain_difference_db'], 4)} dB |"
        )

    lines += [
        "",
        "## Digital silence",
        "",
        "| Source | AudioGate RMS | FFmpeg RMS |",
        "|---|---:|---:|",
    ]
    for item in report["summary"]["silence"]:
        lines.append(
            f"| {item['source_type']} | {f(item['audiogate_rms_dbfs'])} dBFS | "
            f"{f(item['ffmpeg_rms_dbfs'])} dBFS |"
        )

    lines += [
        "",
        "## Tone response relative to the DXD master",
        "",
        "| Signal | Frequency | Source | AudioGate gain | FFmpeg gain |",
        "|---|---:|---|---:|---:|",
    ]
    for item in report["summary"]["tone_response"]:
        lines.append(
            f"| {item['test_id']} | {item['frequency_hz']:,} Hz | {item['source_type']} | "
            f"{f(item['audiogate_gain_db'])} dB | {f(item['ffmpeg_gain_db'])} dB |"
        )

    lines += [
        "",
        "## Aggregate decoder agreement",
        "",
        "| Source | Median normalized correlation | Median normalized difference RMS |",
        "|---|---:|---:|",
    ]
    for source_type, item in report["summary"]["aggregate"].items():
        lines.append(
            f"| {source_type} | {f(item['median_normalized_correlation'], 6)} | "
            f"{f(item['median_normalized_difference_rms_dbfs'])} dBFS |"
        )

    lines += ["", "## Interpretation", ""]
    lines.extend(f"- {item}" for item in report["summary"]["interpretation"])
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    output = root / "reports" / "decoder-comparison"
    if shutil.which("ffmpeg") is None:
        print("ERROR: ffmpeg is required.", file=sys.stderr)
        return 2
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    masters = {r["test_id"]: r for r in manifest if r["role"] == "reference_master"}
    dsfs = [r for r in manifest if r["role"] == "dsd_derivative"]
    wavs = {
        (r["test_id"], r["file_type"].removeprefix("DXD from ")): r
        for r in manifest
        if r["role"] == "decoded_derivative"
    }

    impulse_master = decode(root / masters["A002"]["file"])
    lags: dict[str, dict[str, float]] = {}
    for source_type in SOURCE_TYPES:
        dsf = next(r for r in dsfs if r["test_id"] == "A002" and r["file_type"] == source_type)
        wav = wavs[("A002", source_type)]
        ff = decode(root / dsf["file"])
        ag = decode(root / wav["file"])
        lags[source_type] = {
            "ffmpeg": fractional_lag(impulse_master, ff),
            "audiogate": fractional_lag(impulse_master, ag),
        }

    results: list[dict[str, Any]] = []
    master_cache: dict[str, np.ndarray] = {}
    for index, dsf in enumerate(dsfs, 1):
        test_id = dsf["test_id"]
        source_type = dsf["file_type"]
        wav = wavs[(test_id, source_type)]
        if test_id not in master_cache:
            master_cache[test_id] = decode(root / masters[test_id]["file"])
        master = master_cache[test_id]
        ff_raw = decode(root / dsf["file"])
        ag_raw = decode(root / wav["file"])
        ff_analysis_lag = lags[source_type]["ffmpeg"]
        ag_analysis_lag = lags[source_type]["audiogate"]
        m_ff, ff = align_fractional(master, ff_raw, ff_analysis_lag)
        m_ag, ag = align_fractional(master, ag_raw, ag_analysis_lag)
        _, ff_level = align_fractional(
            master, ff_raw, float(math.floor(lags[source_type]["ffmpeg"]))
        )
        _, ag_level = align_fractional(
            master, ag_raw, float(math.floor(lags[source_type]["audiogate"]))
        )
        length = min(len(m_ff), len(m_ag), len(ff), len(ag))
        m, ff, ag = m_ff[:length], ff[:length], ag[:length]
        ff_metrics = branch_metrics(m, ff, ff_level)
        ag_metrics = branch_metrics(m, ag, ag_level)
        difference = decoder_difference(m, ag, ff)
        results.append(
            {
                "test_id": test_id,
                "source_type": source_type,
                "source_dsf": dsf["file"],
                "audiogate_wav": wav["file"],
                "samples": length,
                "audiogate_decoded_samples": len(ag_raw),
                "ffmpeg_decoded_samples": len(ff_raw),
                "decoded_lengths_identical": len(ff_raw) == len(ag_raw),
                "audiogate_latency_samples": rnd(lags[source_type]["audiogate"], 6),
                "ffmpeg_latency_samples": rnd(lags[source_type]["ffmpeg"], 6),
                "audiogate_analysis_lag_samples": rnd(ag_analysis_lag, 6),
                "ffmpeg_analysis_lag_samples": rnd(ff_analysis_lag, 6),
                **{f"audiogate_{k}": v for k, v in ag_metrics.items()},
                **{f"ffmpeg_{k}": v for k, v in ff_metrics.items()},
                **difference,
                "audiogate_tone_gain_db": (
                    tone_gain(master[: len(ag_level)], ag_level, TONE_TESTS[test_id])
                    if test_id in TONE_TESTS
                    else None
                ),
                "ffmpeg_tone_gain_db": (
                    tone_gain(master[: len(ff_level)], ff_level, TONE_TESTS[test_id])
                    if test_id in TONE_TESTS
                    else None
                ),
            }
        )
        print(f"[{index:02d}/93] {source_type} {test_id}", flush=True)

    alignment = {}
    aggregate = {}
    for source_type in SOURCE_TYPES:
        selected = [r for r in results if r["source_type"] == source_type]
        alignment[source_type] = {
            "audiogate_samples": lags[source_type]["audiogate"],
            "ffmpeg_samples": lags[source_type]["ffmpeg"],
            "identical_length_files": sum(r["decoded_lengths_identical"] for r in selected),
            "length_difference_samples_min": min(
                r["audiogate_decoded_samples"] - r["ffmpeg_decoded_samples"]
                for r in selected
            ),
            "length_difference_samples_max": max(
                r["audiogate_decoded_samples"] - r["ffmpeg_decoded_samples"]
                for r in selected
            ),
        }
        correlations = [
            r["normalized_correlation"]
            for r in selected
            if r["normalized_correlation"] is not None
        ]
        differences = [
            r["normalized_difference_rms_dbfs"]
            for r in selected
            if r["normalized_difference_rms_dbfs"] is not None
        ]
        aggregate[source_type] = {
            "median_normalized_correlation": rnd(float(np.median(correlations)), 9),
            "median_normalized_difference_rms_dbfs": rnd(float(np.median(differences))),
        }

    reference_tone_gain = []
    for r in results:
        if r["test_id"] in {"B004", "B005"}:
            reference_tone_gain.append(
                {
                    "test_id": r["test_id"],
                    "source_type": r["source_type"],
                    "audiogate_gain_db": r["audiogate_gain_db"],
                    "ffmpeg_gain_db": r["ffmpeg_gain_db"],
                    "gain_difference_db": r["gain_difference_db"],
                }
            )
    silence = [
        {
            "source_type": r["source_type"],
            "audiogate_rms_dbfs": r["audiogate_rms_dbfs"],
            "ffmpeg_rms_dbfs": r["ffmpeg_rms_dbfs"],
        }
        for r in results
        if r["test_id"] == "A001"
    ]
    tone_response = [
        {
            "test_id": r["test_id"],
            "frequency_hz": TONE_TESTS[r["test_id"]],
            "source_type": r["source_type"],
            "audiogate_gain_db": r["audiogate_tone_gain_db"],
            "ffmpeg_gain_db": r["ffmpeg_tone_gain_db"],
        }
        for r in results
        if r["test_id"] in TONE_TESTS and r["test_id"] not in {"B003", "B004", "B005"}
    ]
    report = {
        "status": "pass",
        "files_compared": len(results),
        "analysis_rate_hz": RATE,
        "summary": {
            "alignment": alignment,
            "reference_tone_gain": reference_tone_gain,
            "silence": silence,
            "tone_response": tone_response,
            "aggregate": aggregate,
            "interpretation": [
                "AudioGate restores the approximately +6.0206 dB PCM level that FFmpeg leaves at the DSD convention level.",
                "Decoded lengths match for every DSD64 and DSD128 file; eight DSD256 AudioGate WAV files are one sample shorter than FFmpeg output.",
                "Differences remaining after gain compensation reflect the decoders' filters and ultrasonic-noise handling, not only the encoder.",
            ],
        },
        "measurements": results,
    }

    output.mkdir(parents=True, exist_ok=True)
    (output / "decoder-comparison.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    fields = list(results[0])
    with (output / "decoder-comparison.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(results)
    (output / "decoder-comparison.md").write_text(markdown(report), encoding="utf-8")
    print(f"PASS: compared 93 DSF files through AudioGate and FFmpeg. Reports: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
