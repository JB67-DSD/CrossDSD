#!/usr/bin/env python3
"""Add or refresh AudioGate-decoded DXD WAV records in both suite manifests."""

from __future__ import annotations

import csv
import hashlib
import json
import subprocess
from pathlib import Path
from typing import Any


FOLDERS = {
    "DSD64": ("AudioGate64", "AudioGateDecode_DSD64_to_DXD"),
    "DSD128": ("AudioGate128", "AudioGateDecode_DSD128_to_DXD"),
    "DSD256": ("AudioGate256", "AudioGateDecode_DSD256_to_DXD"),
}
FIELDS = [
    "file",
    "test_id",
    "role",
    "source_file",
    "category",
    "container",
    "sample_rate_hz",
    "pcm_bit_depth",
    "channels",
    "encoding",
    "bit_order",
    "duration_s",
    "peak_target_dbfs",
    "encoder",
    "conversion",
    "normalize",
    "title",
    "album",
    "track",
    "disc",
    "date",
    "composer",
    "genre",
    "suite_id",
    "file_type",
    "replaygain_track_gain",
    "replaygain_track_peak",
    "description",
    "notes",
    "sha256",
    "file_size_bytes",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def probe(path: Path) -> tuple[dict[str, Any], dict[str, str]]:
    output = subprocess.run(
        [
            "ffprobe",
            "-v",
            "quiet",
            "-show_entries",
            "format=duration:format_tags:"
            "stream=codec_name,codec_type,sample_rate,channels,bits_per_sample",
            "-of",
            "json",
            str(path),
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    data = json.loads(output)
    stream = next(item for item in data["streams"] if item.get("codec_type") == "audio")
    return data, stream


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    json_path = root / "manifest.json"
    rows = json.loads(json_path.read_text(encoding="utf-8"))
    base_rows = [row for row in rows if row["role"] != "decoded_derivative"]
    masters = {row["test_id"]: row for row in base_rows if row["role"] == "reference_master"}
    dsf_rows = {
        (row["test_id"], row["file_type"]): row
        for row in base_rows
        if row["role"] == "dsd_derivative"
    }
    decoded_rows: list[dict[str, Any]] = []

    for test_id, master in masters.items():
        wav_name = Path(master["file"]).name
        for source_type, (_, wav_folder) in FOLDERS.items():
            source = dsf_rows[(test_id, source_type)]
            relative = f"{wav_folder}/{wav_name}"
            path = root / relative
            data, stream = probe(path)
            tags = data["format"].get("tags", {})
            expected_file_type = f"DXD from {source_type}"
            required_tags = {
                "title",
                "album",
                "track",
                "disc",
                "date",
                "composer",
                "genre",
                "encoder",
                "TSST",
                "TFLT",
                "SOURCE_DSF",
                "SOURCE_FILE_TYPE",
                "CONVERSION",
                "NORMALIZE",
            }
            missing = required_tags - set(tags)
            if missing:
                raise ValueError(f"{relative}: missing metadata {sorted(missing)}")
            if tags["TFLT"] != expected_file_type:
                raise ValueError(f"{relative}: unexpected File Type {tags['TFLT']!r}")
            if tags["SOURCE_DSF"] != source["file"]:
                raise ValueError(f"{relative}: SOURCE_DSF does not match manifest")
            if stream["codec_name"] != "pcm_f32le":
                raise ValueError(f"{relative}: expected pcm_f32le")

            decoded_rows.append(
                {
                    "file": relative,
                    "test_id": test_id,
                    "role": "decoded_derivative",
                    "source_file": source["file"],
                    "category": master["category"],
                    "container": "WAV",
                    "sample_rate_hz": int(stream["sample_rate"]),
                    "pcm_bit_depth": int(stream["bits_per_sample"]),
                    "channels": int(stream["channels"]),
                    "encoding": "IEEE float32 WAV",
                    "bit_order": "",
                    "duration_s": round(float(data["format"]["duration"]), 6),
                    "peak_target_dbfs": master["peak_target_dbfs"],
                    "encoder": tags["encoder"],
                    "conversion": tags["CONVERSION"],
                    "normalize": tags["NORMALIZE"],
                    "title": tags["title"],
                    "album": tags["album"],
                    "track": tags["track"],
                    "disc": tags["disc"],
                    "date": tags["date"],
                    "composer": tags["composer"],
                    "genre": tags["genre"],
                    "suite_id": tags["TSST"],
                    "file_type": tags["TFLT"],
                    "replaygain_track_gain": tags.get("REPLAYGAIN_TRACK_GAIN", ""),
                    "replaygain_track_peak": tags.get("REPLAYGAIN_TRACK_PEAK", ""),
                    "description": f"AudioGate decode of {source_type} derivative. {master['description']}",
                    "notes": (
                        f"Decoded from {source['file']} at 352.8 kHz float32; "
                        "Conversion High Quality; Normalize Off."
                    ),
                    "sha256": sha256(path),
                    "file_size_bytes": path.stat().st_size,
                }
            )

    all_rows: list[dict[str, Any]] = []
    decoded_by_test = {
        (row["test_id"], row["file_type"].removeprefix("DXD from ")): row
        for row in decoded_rows
    }
    for row in base_rows:
        all_rows.append(row)
        if row["role"] == "dsd_derivative":
            all_rows.append(decoded_by_test[(row["test_id"], row["file_type"])])

    if len(all_rows) != 217:
        raise ValueError(f"Expected 217 records, got {len(all_rows)}")
    json_path.write_text(
        json.dumps(all_rows, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    with (root / "manifest.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(all_rows)
    print("Integrated 93 AudioGate-decoded WAV files; manifests now contain 217 records.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
