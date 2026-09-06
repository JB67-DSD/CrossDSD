#!/usr/bin/env python3
"""Rebuild AudioGate-decoded WAV metadata from the corresponding source DSF."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
from pathlib import Path
from typing import Iterable


FOLDERS = {
    "DSD64": ("AudioGate64", "AudioGateDecode_DSD64_to_DXD"),
    "DSD128": ("AudioGate128", "AudioGateDecode_DSD128_to_DXD"),
    "DSD256": ("AudioGate256", "AudioGateDecode_DSD256_to_DXD"),
}


def chunks(data: bytes, start: int = 0) -> Iterable[tuple[bytes, bytes]]:
    position = start
    while position + 8 <= len(data):
        chunk_id = data[position : position + 4]
        size = struct.unpack_from("<I", data, position + 4)[0]
        payload_start = position + 8
        payload_end = payload_start + size
        if payload_end > len(data):
            raise ValueError(f"Truncated RIFF chunk {chunk_id!r}")
        yield chunk_id, data[payload_start:payload_end]
        position = payload_end + (size & 1)


def pack_chunk(chunk_id: bytes, payload: bytes) -> bytes:
    return chunk_id + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")


def synchsafe(value: int) -> bytes:
    return bytes(
        ((value >> 21) & 0x7F, (value >> 14) & 0x7F, (value >> 7) & 0x7F, value & 0x7F)
    )


def id3_frame(frame_id: str, payload: bytes) -> bytes:
    return frame_id.encode("ascii") + len(payload).to_bytes(4, "big") + b"\0\0" + payload


def text_frame(frame_id: str, value: str) -> bytes:
    return id3_frame(frame_id, b"\x00" + value.encode("latin-1"))


def user_text_frame(description: str, value: str) -> bytes:
    return id3_frame(
        "TXXX", b"\x00" + description.encode("latin-1") + b"\0" + value.encode("latin-1")
    )


def preserved_apic(existing_id3: bytes | None) -> bytes | None:
    if not existing_id3 or len(existing_id3) < 10 or existing_id3[:3] != b"ID3":
        return None
    version = existing_id3[3]
    if version not in (3, 4):
        return None
    size = (
        ((existing_id3[6] & 0x7F) << 21)
        | ((existing_id3[7] & 0x7F) << 14)
        | ((existing_id3[8] & 0x7F) << 7)
        | (existing_id3[9] & 0x7F)
    )
    position = 10
    end = min(len(existing_id3), 10 + size)
    while position + 10 <= end:
        frame_id = existing_id3[position : position + 4]
        if not frame_id.strip(b"\0"):
            break
        frame_size = int.from_bytes(existing_id3[position + 4 : position + 8], "big")
        frame_end = position + 10 + frame_size
        if frame_end > end:
            break
        if frame_id == b"APIC":
            return existing_id3[position:frame_end]
        position = frame_end
    return None


def build_id3(tags: dict[str, str], source_dsf: str, source_type: str, apic: bytes | None) -> bytes:
    frames = [
        text_frame("TIT2", tags["title"]),
        text_frame("TALB", tags["album"]),
        text_frame("TRCK", tags["track"]),
        text_frame("TPOS", tags["disc"]),
        text_frame("TYER", tags["date"]),
        text_frame("TCOM", tags["composer"]),
        text_frame("TCON", tags["genre"]),
        text_frame("TSSE", tags["encoder"]),
        text_frame("TSST", tags["TSST"]),
        text_frame("TFLT", f"DXD from {source_type}"),
        user_text_frame("SOURCE_DSF", source_dsf),
        user_text_frame("SOURCE_FILE_TYPE", source_type),
        user_text_frame("CONVERSION", "High Quality"),
        user_text_frame("NORMALIZE", "Off"),
    ]
    for tag in (
        "REPLAYGAIN_TRACK_GAIN",
        "REPLAYGAIN_TRACK_PEAK",
        "REPLAYGAIN_ALBUM_GAIN",
        "REPLAYGAIN_ALBUM_PEAK",
    ):
        if tags.get(tag):
            frames.append(user_text_frame(tag, tags[tag]))
    if apic:
        frames.append(apic)
    body = b"".join(frames)
    return b"ID3\x03\x00\x00" + synchsafe(len(body)) + body


def info_value(value: str) -> bytes:
    encoded = value.encode("utf-8") + b"\0"
    return encoded + (b"\0" if len(encoded) & 1 else b"")


def rebuild_list_info(payload: bytes, id3: bytes, tags: dict[str, str]) -> bytes:
    if payload[:4] != b"INFO":
        raise ValueError("Expected LIST/INFO metadata")
    replacements = {
        b"INAM": tags["title"],
        b"IPRD": tags["album"],
        b"IART": tags["composer"],
        b"IGNR": tags["genre"],
        b"ICRD": tags["date"],
        b"ISFT": tags["encoder"],
    }
    output: list[bytes] = []
    seen: set[bytes] = set()
    for chunk_id, chunk_payload in chunks(payload, 4):
        if chunk_id.lower() == b"id3 ":
            seen.add(b"id3 ")
        elif chunk_id in replacements:
            output.append(pack_chunk(chunk_id, info_value(replacements[chunk_id])))
            seen.add(chunk_id)
        else:
            output.append(pack_chunk(chunk_id, chunk_payload))
    for chunk_id, value in replacements.items():
        if chunk_id not in seen:
            output.append(pack_chunk(chunk_id, info_value(value)))
    return b"INFO" + b"".join(output)


def source_tags(path: Path) -> dict[str, str]:
    output = subprocess.run(
        ["ffprobe", "-v", "quiet", "-show_entries", "format_tags", "-of", "json", str(path)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    tags = json.loads(output).get("format", {}).get("tags", {})
    required = {"title", "album", "track", "disc", "date", "composer", "genre", "encoder", "TSST", "TFLT"}
    missing = required - set(tags)
    if missing:
        raise ValueError(f"{path}: missing source tags {sorted(missing)}")
    return tags


def data_hash(riff: bytes) -> str:
    for chunk_id, payload in chunks(riff, 12):
        if chunk_id == b"data":
            return hashlib.sha256(payload).hexdigest()
    raise ValueError("WAV has no data chunk")


def rebuild(path: Path, tags: dict[str, str], source_dsf: str, source_type: str) -> None:
    original = path.read_bytes()
    if original[:4] != b"RIFF" or original[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    original_audio_hash = data_hash(original)
    output_chunks: list[bytes] = []
    list_seen = False
    existing_id3 = None
    for chunk_id, payload in chunks(original, 12):
        if chunk_id.lower() == b"id3 ":
            existing_id3 = payload
        elif chunk_id == b"LIST" and payload[:4] == b"INFO":
            for info_id, info_payload in chunks(payload, 4):
                if info_id.lower() == b"id3 ":
                    existing_id3 = info_payload
                    break
    id3 = build_id3(tags, source_dsf, source_type, preserved_apic(existing_id3))
    for chunk_id, payload in chunks(original, 12):
        if chunk_id == b"LIST" and payload[:4] == b"INFO":
            payload = rebuild_list_info(payload, id3, tags)
            list_seen = True
        elif chunk_id.lower() == b"id3 ":
            continue
        output_chunks.append(pack_chunk(chunk_id, payload))
    if not list_seen:
        payload = rebuild_list_info(b"INFO", id3, tags)
        output_chunks.append(pack_chunk(b"LIST", payload))
    output_chunks.append(pack_chunk(b"id3 ", id3))
    body = b"WAVE" + b"".join(output_chunks)
    rewritten = b"RIFF" + struct.pack("<I", len(body)) + body
    if data_hash(rewritten) != original_audio_hash:
        raise ValueError(f"{path}: audio payload changed during metadata rewrite")
    temporary = path.with_name(path.name + ".metadata-new")
    temporary.write_bytes(rewritten)
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Benchmark suite directory.",
    )
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    count = 0
    for source_type, (dsf_folder, wav_folder) in FOLDERS.items():
        dsf_files = sorted((root / dsf_folder).glob("*.dsf"))
        wav_files = sorted((root / wav_folder).glob("*.wav"))
        if len(dsf_files) != 31 or len(wav_files) != 31:
            raise ValueError(
                f"{source_type}: expected 31 DSF and 31 WAV, found "
                f"{len(dsf_files)} and {len(wav_files)}"
            )
        for dsf in dsf_files:
            wav = root / wav_folder / dsf.with_suffix(".wav").name
            if not wav.is_file():
                raise ValueError(f"Missing decoded WAV for {dsf}")
            tags = source_tags(dsf)
            rebuild(
                wav,
                tags,
                dsf.relative_to(root).as_posix(),
                source_type,
            )
            count += 1
            print(f"[{count:02d}/93] {wav.relative_to(root)}", flush=True)
    print(f"Rebuilt metadata for {count} WAV files; all audio payload hashes preserved.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
