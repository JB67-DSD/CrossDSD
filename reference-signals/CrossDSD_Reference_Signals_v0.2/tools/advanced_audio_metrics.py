#!/usr/bin/env python3
"""Generate advanced reconstruction, spectral, THD+N and impulse metrics."""

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
FFT_SIZE = 32_768
SOURCE_TYPES = ("DSD64", "DSD128", "DSD256")
TONE_FREQUENCIES = {
    "B001": 20.0,
    "B002": 100.0,
    "B003": 1000.0,
    "B004": 1000.0,
    "B005": 1000.0,
    "B006": 10000.0,
    "B007": 18000.0,
    "B008": 20000.0,
    "B009": 22000.0,
    "B010": 30000.0,
    "B011": 40000.0,
    "B012": 80000.0,
}
ERROR_BANDS = (
    ("0_20k", 0.0, 20_000.0),
    ("20_30k", 20_000.0, 30_000.0),
    ("30_40k", 30_000.0, 40_000.0),
    ("40_50k", 40_000.0, 50_000.0),
    ("50_88k2", 50_000.0, 88_200.0),
    ("88k2_176k4", 88_200.0, 176_400.1),
)
ENERGY_THRESHOLDS = (20_000, 30_000, 40_000, 50_000)
SPECTRAL_TESTS = {"A001", "A002", "B004", "C001", "D001", "F004"}


def db(value: float | None) -> float | None:
    if value is None or value <= 0 or not math.isfinite(value):
        return None
    return 20.0 * math.log10(value)


def rnd(value: float | None, digits: int = 6) -> float | None:
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
    return np.frombuffer(process.stdout, dtype="<f8").reshape(-1, 2)


def gain_normalized_error(master: np.ndarray, decoded: np.ndarray) -> tuple[float | None, np.ndarray]:
    m = master.reshape(-1)
    d = decoded.reshape(-1)
    energy = float(np.dot(m, m))
    if energy <= 1e-24:
        return None, decoded.copy()
    gain = float(np.dot(m, d) / energy)
    if abs(gain) <= 1e-15:
        return gain, decoded.copy()
    return gain, decoded / gain - master


def thdn(samples: np.ndarray, frequency: float) -> float | None:
    if frequency > 20_000:
        return None
    trim = min(int(0.25 * RATE), len(samples) // 8)
    x = samples[trim : len(samples) - trim]
    if len(x) < RATE // 2:
        return None
    n = np.arange(len(x), dtype=np.float64)
    sine = np.sin(2 * np.pi * frequency * n / RATE)
    cosine = np.cos(2 * np.pi * frequency * n / RATE)
    residual_channels = []
    fundamental_levels = []
    for channel in (0, 1):
        signal = x[:, channel]
        centered = signal - float(np.mean(signal))
        a = 2.0 * float(np.dot(centered, sine)) / len(centered)
        b = 2.0 * float(np.dot(centered, cosine)) / len(centered)
        fitted = a * sine + b * cosine
        residual = centered - fitted
        fundamental_rms = math.sqrt((a * a + b * b) / 2.0)
        if fundamental_rms > 0:
            residual_channels.append(residual)
            fundamental_levels.append(fundamental_rms)
    if not residual_channels:
        return None
    residual_stereo = np.column_stack(residual_channels)
    frequencies, power = welch_power(residual_stereo)
    audible = (frequencies >= 20) & (frequencies <= 20_000)
    residual_rms = math.sqrt(float(np.sum(power[audible])))
    return rnd(db(residual_rms / float(np.mean(fundamental_levels))))


def welch_power(samples: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    if len(samples) < FFT_SIZE:
        padded = np.zeros((FFT_SIZE, samples.shape[1]), dtype=np.float64)
        padded[: len(samples)] = samples
        samples = padded
    step = FFT_SIZE // 2
    starts = range(0, len(samples) - FFT_SIZE + 1, step)
    window = np.hanning(FFT_SIZE)
    normalization = FFT_SIZE * float(np.sum(window * window))
    accumulated = np.zeros(FFT_SIZE // 2 + 1, dtype=np.float64)
    count = 0
    for start in starts:
        block = samples[start : start + FFT_SIZE] * window[:, None]
        transformed = np.fft.rfft(block, axis=0)
        power = np.abs(transformed) ** 2 / normalization
        power[1:-1] *= 2.0
        accumulated += np.mean(power, axis=1)
        count += 1
    accumulated /= max(count, 1)
    return np.fft.rfftfreq(FFT_SIZE, 1.0 / RATE), accumulated


def band_rms(freq: np.ndarray, power: np.ndarray, low: float, high: float) -> float | None:
    selected = (freq >= low) & (freq < high)
    return db(math.sqrt(float(np.sum(power[selected]))))


def above_energy(
    freq: np.ndarray, power: np.ndarray, threshold: float
) -> tuple[float | None, float | None]:
    selected = freq >= threshold
    energy = float(np.sum(power[selected]))
    total = float(np.sum(power))
    return db(math.sqrt(energy)), (100.0 * energy / total if total > 0 else None)


def log_spectrum_rows(
    test_id: str,
    source_type: str,
    decoder: str,
    freq: np.ndarray,
    power: np.ndarray,
) -> list[dict[str, Any]]:
    edges = np.geomspace(10.0, RATE / 2, 257)
    rows = []
    for low, high in zip(edges[:-1], edges[1:]):
        selected = (freq >= low) & (freq < high)
        if not np.any(selected):
            continue
        value = float(np.sum(power[selected]))
        rows.append(
            {
                "test_id": test_id,
                "source_type": source_type,
                "decoder": decoder,
                "frequency_hz": rnd(math.sqrt(low * high), 3),
                "error_band_rms_dbfs": rnd(db(math.sqrt(value))),
            }
        )
    return rows


def impulse_metrics(master: np.ndarray, decoded: np.ndarray, lag: float) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    master_position = int(np.argmax(np.abs(master[:, 0])))
    decoded_position = int(np.argmax(np.abs(decoded[:, 0])))
    pre, post = 2048, 8192
    start = decoded_position - pre
    end = decoded_position + post
    response = decoded[max(start, 0) : min(end, len(decoded)), 0]
    if start < 0:
        response = np.pad(response, (-start, 0))
    if end > len(decoded):
        response = np.pad(response, (0, end - len(decoded)))
    input_peak = float(np.max(np.abs(master[:, 0])))
    response = response / input_peak
    raw_energy = response * response
    edge = np.concatenate((raw_energy[:1024], raw_energy[-2048:]))
    stationary_noise_power = float(np.mean(edge))
    significance_threshold = 10.0 * math.sqrt(stationary_noise_power)
    energy = np.where(np.abs(response) >= significance_threshold, raw_energy, 0.0)
    total = float(np.sum(energy))
    pre_energy = float(np.sum(energy[:pre]))
    post_energy = float(np.sum(energy[pre + 1 :]))
    significant = np.flatnonzero(energy > 0)
    low_index = int(significant[0]) if significant.size else pre
    high_index = int(significant[-1]) if significant.size else pre

    circular = np.zeros(65_536, dtype=np.float64)
    circular[:post] = response[pre:]
    circular[-pre:] = response[:pre]
    transform = np.fft.rfft(circular)
    frequencies = np.fft.rfftfreq(65_536, 1.0 / RATE)
    magnitude = np.abs(transform)
    phase = np.unwrap(np.angle(transform))
    omega = 2 * np.pi * frequencies / RATE
    group_delay = -np.gradient(phase, omega)
    magnitude_db = 20 * np.log10(np.maximum(magnitude, 1e-300))
    group_bands = {}
    for label, low, high in (
        ("20_20k", 20, 20_000),
        ("20_40k", 20, 40_000),
        ("40_80k", 40_000, 80_000),
    ):
        selected = (
            (frequencies >= low)
            & (frequencies < high)
            & (magnitude_db > np.max(magnitude_db) - 40)
        )
        group_bands[label] = rnd(float(np.median(group_delay[selected])), 4) if np.any(selected) else None

    metrics = {
        "latency_samples": rnd(lag, 6),
        "latency_us": rnd(lag * 1_000_000 / RATE, 3),
        "peak_sample": decoded_position,
        "peak_amplitude_relative_to_input": rnd(float(np.max(np.abs(response))), 9),
        "pre_ringing_energy_relative_db": rnd(10 * math.log10(pre_energy / total)) if pre_energy > 0 else None,
        "post_ringing_energy_relative_db": rnd(10 * math.log10(post_energy / total)) if post_energy > 0 else None,
        "estimated_stationary_noise_rms": rnd(math.sqrt(stationary_noise_power), 12),
        "significant_response_span_samples": high_index - low_index,
        "significant_response_span_us": rnd((high_index - low_index) * 1_000_000 / RATE, 3),
        "significance_threshold_db_above_noise_rms": 20.0,
        "median_group_delay_samples": group_bands,
    }
    detail = [
        {
            "sample_offset_from_peak": index - pre,
            "time_us": rnd((index - pre) * 1_000_000 / RATE, 3),
            "amplitude_relative_to_input": rnd(float(value), 12),
        }
        for index, value in enumerate(response)
    ]
    return metrics, detail


def write_markdown(path: Path, report: dict[str, Any]) -> None:
    def f(value: Any, digits: int = 3) -> str:
        if value is None:
            return "n/a"
        return f"{value:.{digits}f}" if isinstance(value, float) else str(value)

    lines = [
        "# CrossDSD Advanced Audio Metrics",
        "",
        f"- Status: **{report['status'].upper()}**",
        f"- Decoder branches analyzed: {report['branches_analyzed']}",
        f"- Analysis rate: {RATE:,} Hz",
        f"- Welch FFT size: {FFT_SIZE:,} samples",
        "",
        "## Definitions",
        "",
        "- Reconstruction SNR uses the gain-normalized, time-aligned error against the DXD master.",
        "- Peak error is measured after the same gain normalization and alignment.",
        "- THD+N is measured from 20 Hz to 20 kHz after least-squares removal of the known fundamental and DC.",
        "- Band noise values are RMS levels of the gain-normalized reconstruction error.",
        "- Ultrasonic energy values describe each decoded output, not only its error.",
        "",
        "## 1 kHz reference metrics",
        "",
        "| Signal | Source | Decoder | SNR | Peak error | THD+N | Result |",
        "|---|---|---|---:|---:|---:|---|",
    ]
    for item in report["summary"]["reference_tones"]:
        lines.append(
            f"| {item['test_id']} | {item['source_type']} | {item['decoder']} | "
            f"{f(item['reconstruction_snr_db'])} dB | "
            f"{f(item['peak_error_dbfs'])} dBFS | {f(item['thdn_db'])} dB | "
            f"{item['thdn_status']} |"
        )

    lines += [
        "",
        "## Error noise by band — B004 1 kHz at -20 dBFS",
        "",
        "| Source | Decoder | 0–20 kHz | 20–30 kHz | 30–40 kHz | 40–50 kHz | 50–88.2 kHz | 88.2–176.4 kHz |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for item in report["summary"]["b004_bands"]:
        lines.append(
            f"| {item['source_type']} | {item['decoder']} | "
            f"{f(item['error_0_20k_dbfs'])} | {f(item['error_20_30k_dbfs'])} | "
            f"{f(item['error_30_40k_dbfs'])} | {f(item['error_40_50k_dbfs'])} | "
            f"{f(item['error_50_88k2_dbfs'])} | {f(item['error_88k2_176k4_dbfs'])} |"
        )

    lines += [
        "",
        "## Output energy above frequency thresholds — B004",
        "",
        "| Source | Decoder | >20 kHz | >30 kHz | >40 kHz | >50 kHz |",
        "|---|---|---:|---:|---:|---:|",
    ]
    for item in report["summary"]["b004_bands"]:
        lines.append(
            f"| {item['source_type']} | {item['decoder']} | "
            f"{f(item['output_above_20k_dbfs'])} dBFS | "
            f"{f(item['output_above_30k_dbfs'])} dBFS | "
            f"{f(item['output_above_40k_dbfs'])} dBFS | "
            f"{f(item['output_above_50k_dbfs'])} dBFS |"
        )

    lines += [
        "",
        "## Impulse and group-delay summary",
        "",
        "| Source | Decoder | Latency | Pre-ringing energy | Post-ringing energy | Significant response span | Median GD 20 Hz–20 kHz |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    for item in report["impulse_summary"]:
        lines.append(
            f"| {item['source_type']} | {item['decoder']} | "
            f"{item['latency_samples']} samples | {f(item['pre_ringing_energy_relative_db'])} dB | "
            f"{f(item['post_ringing_energy_relative_db'])} dB | "
            f"{item['significant_response_span_samples']} samples | "
            f"{f(item['median_group_delay_samples']['20_20k'])} samples |"
        )

    lines += [
        "",
        "## Detailed outputs",
        "",
        "- `advanced-metrics.csv`: metrics for all 186 decoder branches.",
        "- `advanced-metrics.json`: complete metrics and methodology.",
        "- `error-spectra.csv`: logarithmically binned error spectra for representative signals.",
        "- `impulse-responses.csv`: detailed impulse responses for all decoder/rate combinations; the reported significant span uses a threshold 20 dB above the stationary noise RMS.",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    output = root / "reports" / "advanced-metrics"
    if shutil.which("ffmpeg") is None:
        print("ERROR: ffmpeg is required.", file=sys.stderr)
        return 2
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    thresholds = json.loads(
        (root / "benchmark_thresholds.json").read_text(encoding="utf-8")
    )
    thdn_limits = thresholds["thdn_1khz_db"]
    masters = {r["test_id"]: r for r in manifest if r["role"] == "reference_master"}
    dsfs = [r for r in manifest if r["role"] == "dsd_derivative"]
    wavs = {
        (r["test_id"], r["file_type"].removeprefix("DXD from ")): r
        for r in manifest
        if r["role"] == "decoded_derivative"
    }

    impulse_master = decode(root / masters["A002"]["file"])
    lags: dict[tuple[str, str], float] = {}
    impulse_raw: dict[tuple[str, str], np.ndarray] = {}
    for source_type in SOURCE_TYPES:
        dsf = next(r for r in dsfs if r["test_id"] == "A002" and r["file_type"] == source_type)
        branches = {
            "AudioGate": decode(root / wavs[("A002", source_type)]["file"]),
            "FFmpeg": decode(root / dsf["file"]),
        }
        for decoder, samples in branches.items():
            lags[(source_type, decoder)] = fractional_lag(impulse_master, samples)
            impulse_raw[(source_type, decoder)] = samples

    results: list[dict[str, Any]] = []
    spectra: list[dict[str, Any]] = []
    master_cache: dict[str, np.ndarray] = {}
    for index, dsf in enumerate(dsfs, 1):
        test_id, source_type = dsf["test_id"], dsf["file_type"]
        if test_id not in master_cache:
            master_cache[test_id] = decode(root / masters[test_id]["file"])
        master = master_cache[test_id]
        branches = {
            "AudioGate": decode(root / wavs[(test_id, source_type)]["file"]),
            "FFmpeg": decode(root / dsf["file"]),
        }
        for decoder, raw in branches.items():
            analysis_lag = lags[(source_type, decoder)]
            m, d = align_fractional(master, raw, analysis_lag)
            m_level, d_level = align_fractional(
                master, raw, float(math.floor(lags[(source_type, decoder)]))
            )
            gain, error = gain_normalized_error(m, d)
            master_rms = float(np.sqrt(np.mean(m * m)))
            error_rms = float(np.sqrt(np.mean(error * error)))
            error_peak = float(np.max(np.abs(error)))
            error_freq, error_power = welch_power(error)
            output_freq, output_power = welch_power(d_level)
            thdn_value = (
                thdn(d_level / gain, TONE_FREQUENCIES[test_id])
                if gain is not None
                and abs(gain) > 1e-15
                and test_id in TONE_FREQUENCIES
                else None
            )
            if thdn_value is None:
                thdn_status = "N/A"
            elif thdn_value <= thdn_limits[source_type]["pass_max"]:
                thdn_status = "PASS"
            elif thdn_value <= thdn_limits[source_type]["warning_max"]:
                thdn_status = "WARNING"
            else:
                thdn_status = "FAIL"
            row: dict[str, Any] = {
                "test_id": test_id,
                "source_type": source_type,
                "decoder": decoder,
                "analysis_lag_samples": rnd(analysis_lag, 6),
                "gain_db": rnd(db(abs(gain))) if gain is not None else None,
                "reconstruction_snr_db": (
                    rnd(-db(error_rms / master_rms))
                    if gain is not None and master_rms > 0 and error_rms > 0
                    else None
                ),
                "error_rms_dbfs": rnd(db(error_rms)),
                "peak_error_dbfs": rnd(db(error_peak)),
                "thdn_db": thdn_value,
                "thdn_status": thdn_status,
            }
            for label, low, high in ERROR_BANDS:
                row[f"error_{label}_dbfs"] = rnd(band_rms(error_freq, error_power, low, high))
            for threshold in ENERGY_THRESHOLDS:
                level, percent = above_energy(output_freq, output_power, threshold)
                key = f"output_above_{threshold // 1000}k"
                row[f"{key}_dbfs"] = rnd(level)
                row[f"{key}_percent"] = rnd(percent, 9)
            results.append(row)
            if test_id in SPECTRAL_TESTS:
                spectra.extend(
                    log_spectrum_rows(test_id, source_type, decoder, error_freq, error_power)
                )
        print(f"[{index:02d}/93] {source_type} {test_id}", flush=True)

    impulse_summary = []
    impulse_detail = []
    for source_type in SOURCE_TYPES:
        for decoder in ("AudioGate", "FFmpeg"):
            metrics, detail = impulse_metrics(
                impulse_master,
                impulse_raw[(source_type, decoder)],
                lags[(source_type, decoder)],
            )
            metrics.update({"source_type": source_type, "decoder": decoder})
            impulse_summary.append(metrics)
            for row in detail:
                row.update({"source_type": source_type, "decoder": decoder})
                impulse_detail.append(row)

    report = {
        "status": "pass",
        "analysis_rate_hz": RATE,
        "welch_fft_size": FFT_SIZE,
        "branches_analyzed": len(results),
        "thresholds": thresholds,
        "methodology": {
            "alignment": "A single deterministic sub-sample latency per decoder and DSD rate is estimated from A002 by parabolic peak interpolation, then compensated with cubic Lagrange interpolation.",
            "gain": "Least-squares projection of aligned decoded PCM onto the DXD master.",
            "snr": "RMS master divided by RMS gain-normalized reconstruction error.",
            "thdn": "20 Hz to 20 kHz residual after least-squares removal of known sine fundamental and DC.",
            "spectra": "50%-overlapped Hann-window Welch power spectra, stereo channels averaged.",
            "group_delay": "Phase derivative of the normalized A002 impulse response with negative-time samples placed at the end of the FFT buffer.",
        },
        "summary": {
            "reference_tones": [
                r for r in results if r["test_id"] in {"B004", "B005"}
            ],
            "b004_bands": [r for r in results if r["test_id"] == "B004"],
        },
        "impulse_summary": impulse_summary,
        "measurements": results,
    }

    output.mkdir(parents=True, exist_ok=True)
    (output / "advanced-metrics.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    with (output / "advanced-metrics.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    with (output / "error-spectra.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(spectra[0]))
        writer.writeheader()
        writer.writerows(spectra)
    with (output / "impulse-responses.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(impulse_detail[0]))
        writer.writeheader()
        writer.writerows(impulse_detail)
    write_markdown(output / "advanced-metrics.md", report)
    print(f"PASS: generated {len(results)} advanced branch measurements in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
