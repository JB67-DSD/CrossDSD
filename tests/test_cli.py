#!/usr/bin/env python3
# Copyright 2026-present, Jean-Luc EVRARD.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# Author Jean-Luc EVRARD

"""End-to-end tests for the publishable CrossDSD command."""

from __future__ import annotations

import json
import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(*arguments: str, success: bool = True) -> subprocess.CompletedProcess[str]:
    process = subprocess.run(arguments, capture_output=True, text=True)
    if success and process.returncode != 0:
        raise AssertionError(process.stderr)
    if not success and process.returncode == 0:
        raise AssertionError("command unexpectedly succeeded")
    return process


def riff_chunks(data: bytes) -> dict[bytes, tuple[int, bytes]]:
    chunks: dict[bytes, tuple[int, bytes]] = {}
    position = 12
    while position + 8 <= len(data):
        name = data[position:position + 4]
        size = struct.unpack_from("<I", data, position + 4)[0]
        payload = data[position + 8:position + 8 + size]
        chunks[name] = (size, payload)
        position += 8 + size + (size & 1)
    return chunks


def write_float_wave(path: Path, frames: int = 7056) -> None:
    samples = b"".join(
        struct.pack("<f", 0.5 * math.sin(2.0 * math.pi * 1000.0 * i / 352_800.0))
        for i in range(frames)
    )
    fmt = struct.pack("<HHIIHH", 3, 1, 352_800, 352_800 * 4, 4, 32)
    fact = struct.pack("<I", frames)
    chunks = (
        b"fmt " + struct.pack("<I", len(fmt)) + fmt
        + b"fact" + struct.pack("<I", len(fact)) + fact
        + b"data" + struct.pack("<I", len(samples)) + samples
    )
    path.write_bytes(
        b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks
    )


def main() -> int:
    tool = Path(sys.argv[1]).resolve()
    suite = Path(sys.argv[2]).resolve()
    assert run(str(tool), "--version").stdout.strip() == "CrossDSD 1.0.0"
    source = suite / "AudioGate64/A002_single_impulse_-6dBFS.dsf"
    with tempfile.TemporaryDirectory() as directory_name:
        directory = Path(directory_name)
        standard = directory / "standard.wav"
        raw = directory / "raw.wav"
        rf64 = directory / "large.wav"
        encode_input = directory / "encode-input.wav"
        encoded_dsf = directory / "encoded.dsf"
        encoded_enhanced = directory / "encoded-enhanced.dsf"
        encoded_dsd128 = directory / "encoded-128.dsf"
        encoded_dsd256 = directory / "encoded-256.dsf"
        encode_report_path = directory / "encode-report.json"
        encode_report_enhanced = directory / "encode-report-enhanced.json"
        encode_report_128 = directory / "encode-report-128.json"
        encode_report_256 = directory / "encode-report-256.json"
        report_path = directory / "report.json"
        analyze_report_path = directory / "analyze-report.json"
        analyze_process = run(
            str(tool), "analyze", str(source),
            "--quicksearch", f"--report={analyze_report_path}",
        )
        analyze_report = json.loads(
            analyze_report_path.read_text(encoding="utf-8")
        )
        assert analyze_report["operation"] == "analyze"
        assert analyze_report["input_dsd_rate"] == 64
        assert analyze_report["reconstructed_rate_hz"] == 352_800
        assert analyze_report["search"] == "quick"
        assert analyze_report["target_peak_dbfs"] == 0
        assert analyze_report["recommended_gain_db"] <= 0
        assert analyze_report["channels"] == 2
        assert analyze_report["reconstructed_peak"] > 0
        assert math.isfinite(analyze_report["reconstructed_peak_dbfs"])
        assert 1 <= analyze_report["peak_channel"] <= 2
        assert 0 <= analyze_report["peak_seconds"] <= analyze_report["duration_seconds"]
        assert len(analyze_report["channel_peaks"]) == 2
        assert "individual gain" in analyze_process.stderr
        batch_report_path = directory / "album-report.json"
        run(
            str(tool), "analyze", str(source), str(source),
            "--album-target=-1", f"--report={batch_report_path}",
        )
        batch_report = json.loads(
            batch_report_path.read_text(encoding="utf-8")
        )
        assert batch_report["operation"] == "analyze-batch"
        assert batch_report["file_count"] == 2
        assert batch_report["target_peak_dbfs"] == -1
        assert batch_report["common_gain_db"] <= 0
        assert len(batch_report["files"]) == 2
        assert batch_report["files"][0]["individual_gain_db"] == \
            batch_report["files"][1]["individual_gain_db"]
        run(
            str(tool), "decode", str(source), str(standard),
            "--backend=cpu", f"--report={report_path}",
        )
        report = json.loads(report_path.read_text(encoding="utf-8"))
        assert report["sample_format"] == "float32"
        assert report["output_rate_hz"] == 352_800
        assert report["profile"] == "standard"
        assert report["container"] == "wav"
        assert report["backend"] == "cpu"
        assert report["output_frames"] > 1_000_000
        data = standard.read_bytes()
        assert data[:4] == b"RIFF"
        chunks = riff_chunks(data)
        assert struct.unpack_from("<H", chunks[b"fmt "][1], 0)[0] == 0xFFFE
        assert struct.unpack_from("<I", chunks[b"fmt "][1], 4)[0] == 352_800
        assert struct.unpack_from("<H", chunks[b"fmt "][1], 14)[0] == 32
        assert struct.unpack_from("<I", chunks[b"fmt "][1], 20)[0] == 0x0003
        assert struct.unpack_from("<I", chunks[b"fmt "][1], 24)[0] == 3
        assert b"id3 " in chunks

        original = standard.read_bytes()
        process = run(
            str(tool), "decode", str(source), str(standard),
            "--backend=cpu", success=False,
        )
        assert "already exists" in process.stderr
        assert standard.read_bytes() == original

        run(
            str(tool), "decode", str(source), str(raw),
            "--backend=cpu", "--profile=raw",
        )
        raw_chunks = riff_chunks(raw.read_bytes())
        assert raw_chunks[b"data"][1] != chunks[b"data"][1]
        assert len(raw_chunks[b"data"][1]) == len(chunks[b"data"][1])

        run(
            str(tool), "decode", str(source), str(rf64),
            "--backend=cpu", "--container=rf64",
        )
        rf64_data = rf64.read_bytes()
        assert rf64_data[:4] == b"RF64"
        assert rf64_data[12:16] == b"ds64"
        assert struct.unpack_from("<Q", rf64_data, 20)[0] == len(rf64_data) - 8

        write_float_wave(encode_input)
        missing_rate = run(
            str(tool), "encode", str(encode_input), str(encoded_dsf),
            success=False,
        )
        assert "usage:" in missing_rate.stderr
        run(
            str(tool), "encode", str(encode_input), str(encoded_dsf),
            "--dsd-rate=64", f"--report={encode_report_path}",
        )
        encode_report = json.loads(
            encode_report_path.read_text(encoding="utf-8")
        )
        assert encode_report["operation"] == "encode"
        assert encode_report["sample_format"] == "float32"
        assert encode_report["input_rate_hz"] == 352_800
        assert encode_report["output_rate_hz"] == 2_822_400
        assert encode_report["dsd_rate"] == "DSD64"
        assert encode_report["encoder_profile"] == "standard"
        assert encode_report["input_frames"] == 7056
        assert encode_report["output_dsd_samples_per_channel"] == 7056 * 8
        dsf = encoded_dsf.read_bytes()
        assert dsf[:4] == b"DSD "
        assert struct.unpack_from("<I", dsf, 56)[0] == 2_822_400
        assert struct.unpack_from("<Q", dsf, 64)[0] == 7056 * 8
        run(
            str(tool), "encode", str(encode_input), str(encoded_enhanced),
            "--dsd-rate=64", "--enhanced",
            f"--report={encode_report_enhanced}",
        )
        enhanced_report = json.loads(
            encode_report_enhanced.read_text(encoding="utf-8")
        )
        assert enhanced_report["encoder_profile"] == "enhanced"
        assert enhanced_report["enhanced_dsd_samples_per_channel"] > 0
        assert enhanced_report["enhanced_segment_count"] >= 1
        ignored_enhanced = directory / "ignored-enhanced.dsf"
        ignored_report_path = directory / "ignored-enhanced.json"
        run(
            str(tool), "encode", str(encode_input),
            str(ignored_enhanced),
            "--dsd-rate=128", "--enhanced",
            f"--report={ignored_report_path}",
        )
        ignored_report = json.loads(
            ignored_report_path.read_text(encoding="utf-8")
        )
        assert ignored_report["dsd_rate"] == "DSD128"
        assert ignored_report["encoder_profile"] == "standard"
        ignored_report_256_path = directory / "ignored-enhanced-256.json"
        run(
            str(tool), "encode", str(encode_input),
            str(directory / "ignored-enhanced-256.dsf"),
            "--dsd-rate=256", "--enhanced",
            f"--report={ignored_report_256_path}",
        )
        ignored_report_256 = json.loads(
            ignored_report_256_path.read_text(encoding="utf-8")
        )
        assert ignored_report_256["dsd_rate"] == "DSD256"
        assert ignored_report_256["encoder_profile"] == "standard"
        run(
            str(tool), "encode", str(encode_input), str(encoded_dsd128),
            "--dsd-rate=128", f"--report={encode_report_128}",
        )
        report128 = json.loads(
            encode_report_128.read_text(encoding="utf-8")
        )
        assert report128["output_rate_hz"] == 5_644_800
        assert report128["dsd_rate"] == "DSD128"
        assert report128["output_dsd_samples_per_channel"] == 7056 * 16
        assert report128["output_packed_bytes_per_channel"] == 7056 * 2
        dsf128 = encoded_dsd128.read_bytes()
        assert struct.unpack_from("<I", dsf128, 56)[0] == 5_644_800
        assert struct.unpack_from("<Q", dsf128, 64)[0] == 7056 * 16
        run(
            str(tool), "encode", str(encode_input), str(encoded_dsd256),
            "--dsd-rate=256", f"--report={encode_report_256}",
        )
        report256 = json.loads(
            encode_report_256.read_text(encoding="utf-8")
        )
        assert report256["output_rate_hz"] == 11_289_600
        assert report256["dsd_rate"] == "DSD256"
        assert report256["output_dsd_samples_per_channel"] == 7056 * 32
        assert report256["output_packed_bytes_per_channel"] == 7056 * 4
        dsf256 = encoded_dsd256.read_bytes()
        assert struct.unpack_from("<I", dsf256, 56)[0] == 11_289_600
        assert struct.unpack_from("<Q", dsf256, 64)[0] == 7056 * 32

        transcoded = directory / "transcoded-128.dsf"
        transcode_report_path = directory / "transcode-report.json"
        run(
            str(tool), "transcode", str(encoded_dsf), str(transcoded),
            "--dsd-rate=128", "--maxpeak=-1",
            f"--report={transcode_report_path}",
        )
        transcode_report = json.loads(
            transcode_report_path.read_text(encoding="utf-8")
        )
        assert transcode_report["input_dsd_rate"] == 64
        assert transcode_report["output_dsd_rate"] == 128
        assert transcode_report["max_peak_enabled"] is True
        assert transcode_report["requested_max_peak_dbfs"] == -1
        assert transcode_report["applied_gain_db"] <= 0
        transcoded_dsf = transcoded.read_bytes()
        assert struct.unpack_from("<I", transcoded_dsf, 56)[0] == 5_644_800
        assert struct.unpack_from("<Q", transcoded_dsf, 64)[0] == 7056 * 16

        invalid_peak = run(
            str(tool), "transcode", str(encoded_dsf),
            str(directory / "invalid-peak.dsf"), "--dsd-rate=64",
            "--maxpeak=0.1", success=False,
        )
        assert "usage:" in invalid_peak.stderr
    print("All CrossDSD CLI integration tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
