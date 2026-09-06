#!/usr/bin/env python3
"""Validate the CrossDSD Benchmark Suite corpus and manifests."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any


AUDIO_EXTENSIONS = {".wav", ".dsf"}
REQUIRED_FIELDS = {
    "file",
    "test_id",
    "role",
    "source_file",
    "container",
    "sample_rate_hz",
    "pcm_bit_depth",
    "channels",
    "encoding",
    "bit_order",
    "duration_s",
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
    "sha256",
    "file_size_bytes",
}
REQUIRED_DSF_TAGS = {
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
}
DSD_SAMPLE_RATES = {
    "DSD64": 2_822_400,
    "DSD128": 5_644_800,
    "DSD256": 11_289_600,
}
NUMERIC_MANIFEST_FIELDS = {
    "sample_rate_hz",
    "pcm_bit_depth",
    "channels",
    "duration_s",
    "file_size_bytes",
}


@dataclass
class Finding:
    severity: str
    code: str
    file: str
    message: str


class Validator:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.findings: list[Finding] = []
        self.checked_files = 0

    def error(self, code: str, message: str, file: str = "") -> None:
        self.findings.append(Finding("error", code, file, message))

    def warning(self, code: str, message: str, file: str = "") -> None:
        self.findings.append(Finding("warning", code, file, message))

    def validate(self) -> dict[str, Any]:
        json_rows = self._load_json_manifest()
        csv_rows = self._load_csv_manifest()
        self._compare_manifests(json_rows, csv_rows)
        self._validate_inventory(json_rows)
        self._validate_checksum_index()

        errors = sum(f.severity == "error" for f in self.findings)
        warnings = sum(f.severity == "warning" for f in self.findings)
        return {
            "suite_root": str(self.root),
            "status": "pass" if errors == 0 else "fail",
            "records": len(json_rows),
            "audio_files_checked": self.checked_files,
            "errors": errors,
            "warnings": warnings,
            "findings": [asdict(f) for f in self.findings],
        }

    def _load_json_manifest(self) -> list[dict[str, Any]]:
        path = self.root / "manifest.json"
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            self.error("manifest_json_unreadable", str(exc), "manifest.json")
            return []
        if not isinstance(data, list):
            self.error("manifest_json_shape", "The JSON root must be an array.", "manifest.json")
            return []
        return [row for row in data if isinstance(row, dict)]

    def _load_csv_manifest(self) -> list[dict[str, str]]:
        path = self.root / "manifest.csv"
        try:
            with path.open(encoding="utf-8", newline="") as stream:
                return list(csv.DictReader(stream))
        except OSError as exc:
            self.error("manifest_csv_unreadable", str(exc), "manifest.csv")
            return []

    @staticmethod
    def _canonical(value: Any) -> str:
        if value is None:
            return ""
        if isinstance(value, float):
            return format(value, ".12g")
        return str(value)

    @classmethod
    def _values_equal(cls, field: str, left: Any, right: Any) -> bool:
        if field in NUMERIC_MANIFEST_FIELDS:
            if cls._canonical(left) == cls._canonical(right) == "":
                return True
            try:
                return float(left) == float(right)
            except (TypeError, ValueError):
                return False
        return cls._canonical(left) == cls._canonical(right)

    def _compare_manifests(
        self, json_rows: list[dict[str, Any]], csv_rows: list[dict[str, str]]
    ) -> None:
        json_by_file: dict[str, dict[str, Any]] = {}
        csv_by_file: dict[str, dict[str, str]] = {}
        for label, rows, target in (
            ("JSON", json_rows, json_by_file),
            ("CSV", csv_rows, csv_by_file),
        ):
            for index, row in enumerate(rows, 1):
                missing = REQUIRED_FIELDS - set(row)
                if missing:
                    self.error(
                        "manifest_fields_missing",
                        f"{label} record {index} lacks: {', '.join(sorted(missing))}.",
                        self._canonical(row.get("file")),
                    )
                name = self._canonical(row.get("file"))
                if not name:
                    self.error("manifest_file_empty", f"{label} record {index} has no file.")
                elif name in target:
                    self.error("manifest_duplicate", f"{label} contains a duplicate record.", name)
                else:
                    target[name] = row

        json_files = set(json_by_file)
        csv_files = set(csv_by_file)
        for name in sorted(json_files - csv_files):
            self.error("manifest_csv_missing_record", "Present in JSON but absent from CSV.", name)
        for name in sorted(csv_files - json_files):
            self.error("manifest_json_missing_record", "Present in CSV but absent from JSON.", name)

        for name in sorted(json_files & csv_files):
            json_row = json_by_file[name]
            csv_row = csv_by_file[name]
            for field in sorted(set(json_row) | set(csv_row)):
                if not self._values_equal(field, json_row.get(field), csv_row.get(field)):
                    self.error(
                        "manifest_value_mismatch",
                        f"Field {field!r} differs between JSON and CSV.",
                        name,
                    )

    def _validate_inventory(self, rows: list[dict[str, Any]]) -> None:
        manifest_files = {self._canonical(row.get("file")) for row in rows}
        disk_files = {
            path.relative_to(self.root).as_posix()
            for path in self.root.rglob("*")
            if path.is_file() and path.suffix.lower() in AUDIO_EXTENSIONS
        }
        for name in sorted(disk_files - manifest_files):
            self.error("audio_not_in_manifest", "Audio file is not inventoried.", name)
        for name in sorted(manifest_files - disk_files):
            self.error("manifest_audio_missing", "Inventoried audio file is missing.", name)

        for row in rows:
            name = self._canonical(row.get("file"))
            if not name or name not in disk_files:
                continue
            self.checked_files += 1
            self._validate_audio_row(row, self.root / name)

    def _validate_audio_row(self, row: dict[str, Any], path: Path) -> None:
        name = path.relative_to(self.root).as_posix()
        expected_size = self._integer(row.get("file_size_bytes"), "file_size_bytes", name)
        if expected_size is not None and path.stat().st_size != expected_size:
            self.error(
                "file_size_mismatch",
                f"Expected {expected_size} bytes, found {path.stat().st_size}.",
                name,
            )

        expected_sha = self._canonical(row.get("sha256")).lower()
        actual_sha = self._sha256(path)
        if expected_sha != actual_sha:
            self.error("sha256_mismatch", f"Expected {expected_sha}, found {actual_sha}.", name)

        probe = self._probe(path)
        if probe is None:
            return
        streams = [s for s in probe.get("streams", []) if s.get("codec_type") == "audio"]
        if len(streams) != 1:
            self.error("audio_stream_count", f"Expected one audio stream, found {len(streams)}.", name)
            return
        stream = streams[0]
        container = self._canonical(row.get("container")).upper()
        if path.suffix.lower() == ".wav":
            self._validate_wav(row, probe, stream, name, container)
        elif path.suffix.lower() == ".dsf":
            self._validate_dsf(row, probe, stream, name, container)

    def _validate_wav(
        self,
        row: dict[str, Any],
        probe: dict[str, Any],
        stream: dict[str, Any],
        name: str,
        container: str,
    ) -> None:
        if container != "WAV" or "wav" not in probe.get("format", {}).get("format_name", ""):
            self.error("container_mismatch", f"Expected WAV, manifest says {container!r}.", name)
        if stream.get("codec_name") != "pcm_f32le":
            self.error("wav_codec", f"Expected pcm_f32le, found {stream.get('codec_name')!r}.", name)
        self._compare_integer(row, "sample_rate_hz", stream.get("sample_rate"), name)
        self._compare_integer(row, "pcm_bit_depth", stream.get("bits_per_sample"), name)
        self._compare_integer(row, "channels", stream.get("channels"), name)
        self._compare_duration(row, probe, name)
        role = self._canonical(row.get("role"))
        file_type = self._canonical(row.get("file_type"))
        if role == "reference_master":
            if file_type != "DXD":
                self.error("wav_file_type", "Reference WAV File Type must be DXD.", name)
        elif role == "decoded_derivative":
            if file_type not in {
                "DXD from DSD64",
                "DXD from DSD128",
                "DXD from DSD256",
            }:
                self.error(
                    "wav_file_type",
                    f"Decoded derivative has unexpected File Type {file_type!r}.",
                    name,
                )
            source = self._canonical(row.get("source_file"))
            if not source or not (self.root / source).is_file():
                self.error("wav_source_missing", f"Source DSF {source!r} does not exist.", name)
            tags = probe.get("format", {}).get("tags", {})
            mapping = {
                "title": "title",
                "album": "album",
                "track": "track",
                "disc": "disc",
                "date": "date",
                "composer": "composer",
                "genre": "genre",
                "encoder": "encoder",
                "TSST": "suite_id",
                "TFLT": "file_type",
                "CONVERSION": "conversion",
                "NORMALIZE": "normalize",
            }
            for tag, field in mapping.items():
                if self._canonical(tags.get(tag)) != self._canonical(row.get(field)):
                    self.error(
                        "wav_tag_mismatch",
                        f"Embedded tag {tag!r} differs from manifest field {field!r}.",
                        name,
                    )
            if self._canonical(tags.get("SOURCE_DSF")) != source:
                self.error(
                    "wav_source_tag_mismatch",
                    "Embedded SOURCE_DSF differs from manifest source_file.",
                    name,
                )
        else:
            self.error("wav_role", f"Unexpected WAV role {role!r}.", name)

    def _validate_dsf(
        self,
        row: dict[str, Any],
        probe: dict[str, Any],
        stream: dict[str, Any],
        name: str,
        container: str,
    ) -> None:
        if container != "DSF" or "dsf" not in probe.get("format", {}).get("format_name", ""):
            self.error("container_mismatch", f"Expected DSF, manifest says {container!r}.", name)
        if stream.get("codec_name") != "dsd_lsbf_planar":
            self.error(
                "dsf_codec",
                f"Expected LSB-first planar DSD, found {stream.get('codec_name')!r}.",
                name,
            )
        self._compare_integer(row, "channels", stream.get("channels"), name)
        self._compare_duration(row, probe, name)
        if self._canonical(row.get("role")) != "dsd_derivative":
            self.error("dsf_role", "DSF must have role dsd_derivative.", name)
        if self._canonical(row.get("bit_order")) != "LSB-first":
            self.error("dsf_bit_order", "Manifest bit order must be LSB-first.", name)

        tags = probe.get("format", {}).get("tags", {})
        for tag in sorted(REQUIRED_DSF_TAGS):
            if not self._canonical(tags.get(tag)):
                self.error("dsf_tag_missing", f"Required embedded tag {tag!r} is absent.", name)

        tag_mapping = {
            "title": "title",
            "album": "album",
            "track": "track",
            "disc": "disc",
            "date": "date",
            "composer": "composer",
            "genre": "genre",
            "encoder": "encoder",
            "TSST": "suite_id",
            "TFLT": "file_type",
            "REPLAYGAIN_TRACK_GAIN": "replaygain_track_gain",
            "REPLAYGAIN_TRACK_PEAK": "replaygain_track_peak",
        }
        for tag, field in tag_mapping.items():
            if self._canonical(tags.get(tag)) != self._canonical(row.get(field)):
                self.error(
                    "dsf_tag_mismatch",
                    f"Embedded tag {tag!r} differs from manifest field {field!r}.",
                    name,
                )

        file_type = self._canonical(tags.get("TFLT"))
        expected_rate = DSD_SAMPLE_RATES.get(file_type)
        manifest_rate = self._integer(row.get("sample_rate_hz"), "sample_rate_hz", name)
        if expected_rate is None:
            self.error("dsf_file_type", f"Unsupported File Type {file_type!r}.", name)
        elif manifest_rate is not None and manifest_rate != expected_rate:
            self.error(
                "dsf_rate_mismatch",
                f"{file_type} requires {expected_rate} Hz, manifest has {manifest_rate}.",
                name,
            )

        source = self._canonical(row.get("source_file"))
        if not source or not (self.root / source).is_file():
            self.error("dsf_source_missing", f"DXD source {source!r} does not exist.", name)

    def _validate_checksum_index(self) -> None:
        path = self.root / "SHA256SUMS.txt"
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            self.error("checksum_index_unreadable", str(exc), "SHA256SUMS.txt")
            return
        indexed: dict[str, str] = {}
        for number, line in enumerate(lines, 1):
            try:
                digest, name = line.split(maxsplit=1)
            except ValueError:
                self.error(
                    "checksum_index_syntax",
                    f"Malformed checksum line {number}.",
                    "SHA256SUMS.txt",
                )
                continue
            name = name.lstrip("*").removeprefix("./")
            if name in indexed:
                self.error("checksum_index_duplicate", "Duplicate checksum entry.", name)
            indexed[name] = digest.lower()

        distributed = {
            p.relative_to(self.root).as_posix()
            for p in self.root.rglob("*")
            if p.is_file()
            and p.name not in {"SHA256SUMS.txt", ".DS_Store"}
            and not p.name.endswith(".new")
        }
        required = {
            name
            for name in distributed
            if Path(name).suffix.lower() in AUDIO_EXTENSIONS
            or name
            in {
                "README.md",
                "generation_constants.py",
                "manifest.csv",
                "manifest.json",
                "tools/validate_benchmark.py",
            }
        }
        for name in sorted(required - set(indexed)):
            self.error("checksum_entry_missing", "File is absent from SHA256SUMS.txt.", name)
        for name in sorted(set(indexed) - distributed):
            self.error("checksum_entry_orphan", "Checksum refers to a missing file.", name)
        for name in sorted(distributed & set(indexed)):
            actual = self._sha256(self.root / name)
            if indexed[name] != actual:
                self.error(
                    "checksum_index_mismatch",
                    f"Index has {indexed[name]}, actual value is {actual}.",
                    name,
                )

    def _probe(self, path: Path) -> dict[str, Any] | None:
        try:
            cp = subprocess.run(
                [
                    "ffprobe",
                    "-v",
                    "quiet",
                    "-show_entries",
                    "format=format_name,duration:format_tags:"
                    "stream=codec_name,codec_type,sample_rate,channels,bits_per_sample",
                    "-of",
                    "json",
                    str(path),
                ],
                capture_output=True,
                text=True,
                check=True,
            )
            return json.loads(cp.stdout)
        except (subprocess.CalledProcessError, json.JSONDecodeError) as exc:
            self.error("ffprobe_failed", str(exc), path.relative_to(self.root).as_posix())
            return None

    def _compare_integer(
        self, row: dict[str, Any], field: str, actual: Any, name: str
    ) -> None:
        expected = self._integer(row.get(field), field, name)
        actual_int = self._integer(actual, f"detected {field}", name)
        if expected is not None and actual_int is not None and expected != actual_int:
            self.error(
                f"{field}_mismatch",
                f"Manifest has {expected}, detected value is {actual_int}.",
                name,
            )

    def _compare_duration(
        self, row: dict[str, Any], probe: dict[str, Any], name: str
    ) -> None:
        try:
            expected = float(row.get("duration_s"))
            actual = float(probe.get("format", {}).get("duration"))
        except (TypeError, ValueError):
            self.error("duration_invalid", "Duration is absent or non-numeric.", name)
            return
        if abs(expected - actual) > 0.00001:
            self.error(
                "duration_mismatch",
                f"Manifest has {expected:.6f} s, detected value is {actual:.6f} s.",
                name,
            )

    def _integer(self, value: Any, field: str, name: str) -> int | None:
        try:
            return int(value)
        except (TypeError, ValueError):
            self.error("integer_invalid", f"{field!r} is absent or non-integer.", name)
            return None

    @staticmethod
    def _sha256(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate the CrossDSD corpus, manifests, metadata and checksums."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Benchmark suite directory (default: parent of this script's tools directory).",
    )
    parser.add_argument(
        "--json-report",
        type=Path,
        help="Optionally write the complete validation report as JSON.",
    )
    parser.add_argument("--quiet", action="store_true", help="Only print the final summary.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.expanduser().resolve()
    if shutil.which("ffprobe") is None:
        print("ERROR: ffprobe is required but was not found in PATH.", file=sys.stderr)
        return 2
    if not root.is_dir():
        print(f"ERROR: benchmark directory does not exist: {root}", file=sys.stderr)
        return 2

    report = Validator(root).validate()
    if args.json_report:
        args.json_report.parent.mkdir(parents=True, exist_ok=True)
        args.json_report.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )

    if not args.quiet:
        for finding in report["findings"]:
            location = f" [{finding['file']}]" if finding["file"] else ""
            print(
                f"{finding['severity'].upper()}: {finding['code']}{location}: "
                f"{finding['message']}"
            )
    print(
        f"{report['status'].upper()}: {report['audio_files_checked']} audio files, "
        f"{report['records']} manifest records, {report['errors']} errors, "
        f"{report['warnings']} warnings."
    )
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
