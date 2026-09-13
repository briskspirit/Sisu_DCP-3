#!/usr/bin/env python3
"""Dump Nokia DCT-3 PPM chunks and export TONE resources.

The raw resource-preserving output is the chunk/subchunk `.bin` files. For the
TONE chunk this also writes NokiX-compatible `.re` files and simple MIDI preview
files. The MIDI output is for listening/convenience; the `.bin`/`.re` files are
the precise artifacts to keep.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import wave
from pathlib import Path
from typing import Callable


PPM_FULLFLASH_OFFSET = 0x0F0000
FIRMWARE_BASE = 0x00200000
NOTE_FREQUENCY_TABLE = 0x002D9FE8
TONE_UNIT_HZ = 125.4875
MIDI_TPQ = 480
MIDI_TEMPO_US = 500_000
MIDI_TICKS_PER_SECOND = MIDI_TPQ * 1_000_000 / MIDI_TEMPO_US
WAV_SAMPLE_RATE = 44100
WAV_AMPLITUDE = 11200
WAV_RENDERER = "speaker_soft_sine_register_preview"
NORMAL_PITCH_MIN = 0x41
NORMAL_PITCH_MAX = 0xA4
DYNAMIC_PITCH_COMMAND = 0xA6

PPM_TONE_STARTUP_MELODY_STATUS = {
    "status": "ruled_out_by_hardware",
    "scope": "all extracted PPM TONE / ringtone-menu resources",
    "reason": "Hardware comparison: user checked the extracted tune list and reports the startup melody is not one of these tones.",
}


def be32(data: bytes) -> int:
    return int.from_bytes(data, "big")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def clean_type(data: bytes) -> str:
    return data.rstrip(b"\x00 ").decode("latin1", "replace")


def sanitize(text: str) -> str:
    text = re.sub(r"[^A-Za-z0-9._ -]+", "_", text).strip(" ._")
    return text or "unnamed"


def locate_ppm(data: bytes) -> int:
    if len(data) == 0x200000 and data[PPM_FULLFLASH_OFFSET:PPM_FULLFLASH_OFFSET + 4] == b"PPM\x00":
        return PPM_FULLFLASH_OFFSET
    if data[:4] == b"PPM\x00":
        return 0
    if data[:4] == b"~PPM":
        return 1
    pos = data.find(b"PPM\x00V ")
    if pos >= 0:
        return pos
    pos = data.find(b"~PPM\x00V ")
    if pos >= 0:
        return pos + 1
    raise ValueError("Could not locate PPM header")


def parse_ppm(data: bytes) -> tuple[int, list[dict[str, object]]]:
    ppm_start = locate_ppm(data)
    offset = ppm_start + 44
    chunks: list[dict[str, object]] = []

    while offset + 20 <= len(data):
        chunk_id = be32(data[offset:offset + 4])
        length = be32(data[offset + 4:offset + 8])
        chunk_type = clean_type(data[offset + 8:offset + 12])
        if not chunk_type or length <= 0 or offset + length > len(data):
            break

        subchunks = []
        suboff = offset + 20
        chunk_end = offset + length
        while suboff + 12 <= chunk_end:
            sub_id = be32(data[suboff:suboff + 4])
            sub_len = be32(data[suboff + 4:suboff + 8])
            sub_type = clean_type(data[suboff + 8:suboff + 12])
            if sub_id == 0 or sub_len <= 0 or not sub_type:
                break
            if suboff + sub_len > chunk_end:
                break
            payload = data[suboff:suboff + sub_len]
            subchunks.append(
                {
                    "id": sub_id,
                    "type": sub_type,
                    "offset": suboff,
                    "length": sub_len,
                    "payload": payload,
                }
            )
            suboff += sub_len + ((4 - sub_len % 4) % 4)

        chunks.append(
            {
                "id": chunk_id,
                "type": chunk_type,
                "offset": offset,
                "length": length,
                "subchunks": subchunks,
            }
        )
        offset += length + (length % 2)

    return ppm_start, chunks


def midi_vlq(value: int) -> bytes:
    value = max(0, int(value))
    parts = [value & 0x7F]
    value >>= 7
    while value:
        parts.append(0x80 | (value & 0x7F))
        value >>= 7
    return bytes(reversed(parts))


def midi_note_from_nokia_pitch(pitch: int) -> int | None:
    if not NORMAL_PITCH_MIN <= pitch <= NORMAL_PITCH_MAX:
        return None
    return max(0, min(127, pitch - 54))


def firmware_note_frequency_table(data: bytes) -> dict[int, int]:
    """Return the firmware note-frequency table keyed by Nokia pitch byte."""

    table_off = NOTE_FREQUENCY_TABLE - FIRMWARE_BASE
    if table_off < 0 or table_off >= len(data):
        return {}
    out: dict[int, int] = {}
    for pitch in range(0x40, NORMAL_PITCH_MAX + 1):
        off = table_off + 2 * (pitch - 0x40)
        if off + 2 > len(data):
            break
        freq = int.from_bytes(data[off:off + 2], "big")
        if freq > 0:
            out[pitch] = freq
    return out


def pitch_to_midi_from_frequency_table(table: dict[int, int]) -> Callable[[int], int | None]:
    def mapper(pitch: int) -> int | None:
        freq = table.get(pitch)
        if freq:
            return max(0, min(127, round(69 + 12 * math.log2(freq / 440.0))))
        return midi_note_from_nokia_pitch(pitch)

    return mapper


def pitch_to_hz_from_frequency_table(table: dict[int, int]) -> Callable[[int], float | None]:
    pitch_to_midi = pitch_to_midi_from_frequency_table(table)

    def mapper(pitch: int) -> float | None:
        freq = table.get(pitch)
        if freq:
            return float(freq)
        note = pitch_to_midi(pitch)
        if note is None:
            return None
        return midi_note_hz(note)

    return mapper


def tone_start_offset(tone_data: bytes) -> int:
    if len(tone_data) >= 5 and tone_data[:3] == b"\x00\x00\x02" and tone_data[3] in {0xFC, 0xFD, 0xFE, 0xFF}:
        return 5
    if len(tone_data) >= 3 and tone_data[:2] == b"\x00\x02" and tone_data[2] in {0xFC, 0xFD, 0xFE, 0xFF}:
        return 4 if len(tone_data) >= 4 and tone_data[3] == 0x09 else 3
    if len(tone_data) >= 3 and tone_data[:2] in {b"\x00\x01", b"\x00\x09"} and tone_data[2] in {0xFC, 0xFD, 0xFE, 0xFF}:
        return 3
    if len(tone_data) >= 3 and tone_data[:2] in {b"\x00\x01", b"\x00\x09"}:
        return 2
    if len(tone_data) >= 2 and tone_data[0] == 0x00 and (
        tone_data[1] == 0x40
        or NORMAL_PITCH_MIN <= tone_data[1] <= NORMAL_PITCH_MAX
        or tone_data[1] in {DYNAMIC_PITCH_COMMAND, 0x05, 0x06, 0x0A}
    ):
        return 1
    return 0


def tone_ticks(units: int) -> int:
    return max(1, round(units * MIDI_TICKS_PER_SECOND / TONE_UNIT_HZ))


def tone_raw_events_with_end(
    tone_data: bytes,
    start: int | None = None,
) -> tuple[list[tuple[int, int | None, int]], int, str]:
    """Return decoded events, end offset, and the terminator/reason.

    Nokia tone data is bytecode, not PCM. The common stock/NokiX commands are
    ``05 xx`` repeat-start, ``06`` repeat-end, ``0a xx`` vibra/preview marker,
    ``07 0b`` repeat-forever marker, and ``0b`` stream end.
    """
    initial = tone_start_offset(tone_data) if start is None else start

    def parse(pos: int, in_repeat: bool = False) -> tuple[list[tuple[int, int | None, int]], int, str]:
        events: list[tuple[int, int | None, int]] = []
        pending_rest = 0

        def flush_rest() -> None:
            nonlocal pending_rest
            if pending_rest:
                events.append((pending_rest, None, 0))
                pending_rest = 0

        while pos < len(tone_data):
            cmd = tone_data[pos]
            if cmd == 0x0B:
                flush_rest()
                return events, pos + 1, "end"
            if cmd == 0x07:
                if pos + 1 < len(tone_data) and tone_data[pos + 1] == 0x0B:
                    flush_rest()
                    return events, pos + 2, "repeat_forever"
                if pos + 1 >= len(tone_data):
                    flush_rest()
                    return events, pos + 1, "repeat_forever_truncated"
            if cmd == 0x0A and pos + 1 < len(tone_data):
                pos += 2
                continue
            if cmd == 0x05 and pos + 1 < len(tone_data):
                repeat_count = tone_data[pos + 1]
                if repeat_count < 2:
                    pos += 2
                    continue
                flush_rest()
                block_events, pos, status = parse(pos + 2, True)
                for _ in range(repeat_count):
                    events.extend(block_events)
                if status != "end_repeat":
                    flush_rest()
                    return events, pos, status
                continue
            if cmd == 0x06:
                if not in_repeat:
                    pos += 1
                    continue
                flush_rest()
                return events, pos + 1, "end_repeat"
            if cmd in {0x09, 0x0C, 0x0D, 0x0F, 0x10}:
                pos += 1
                continue
            if cmd in {0x01, 0x02, 0x08, 0x0E} and pos + 1 < len(tone_data):
                pos += 2
                continue
            if pos + 1 >= len(tone_data):
                flush_rest()
                return events, pos, "truncated_pair"

            pitch = tone_data[pos]
            duration = tone_ticks(tone_data[pos + 1])
            if pitch == 0x40:
                pending_rest += duration
            elif NORMAL_PITCH_MIN <= pitch <= NORMAL_PITCH_MAX:
                events.append((pending_rest, pitch, duration))
                pending_rest = 0
            else:
                pending_rest += duration
            pos += 2

        flush_rest()
        return events, pos, "eof"

    return parse(initial)


def tone_raw_events(tone_data: bytes) -> list[tuple[int, int | None, int]]:
    """Return (delta_ticks, raw_pitch_or_none, duration_ticks) events."""
    events, _end, _status = tone_raw_events_with_end(tone_data)
    return events


def trim_trailing_preview_rest(events: list[tuple[int, int | None, int]]) -> list[tuple[int, int | None, int]]:
    """Drop terminal silent flush events from convenience previews only."""
    preview_events = list(events)
    while preview_events and preview_events[-1][1] is None and preview_events[-1][2] == 0:
        preview_events.pop()
    return preview_events


def tone_preview_raw_events(tone_data: bytes, repeat_cycles: int = 1) -> list[tuple[int, int | None, int]]:
    raw_events, _end, status = tone_raw_events_with_end(tone_data)
    cycles = max(1, int(repeat_cycles))
    if cycles > 1 and status in {"repeat_forever", "repeat_forever_truncated"}:
        events: list[tuple[int, int | None, int]] = []
        for cycle in range(cycles):
            cycle_events = raw_events if cycle < cycles - 1 else trim_trailing_preview_rest(raw_events)
            events.extend(cycle_events)
        return events
    return trim_trailing_preview_rest(raw_events)


def _map_tone_events(
    raw_events: list[tuple[int, int | None, int]],
    pitch_to_midi: Callable[[int], int | None] = midi_note_from_nokia_pitch,
) -> list[tuple[int, int | None, int]]:
    events: list[tuple[int, int | None, int]] = []
    pending_rest = 0
    for delta, pitch, duration in raw_events:
        pending_rest += delta
        if pitch is None:
            events.append((pending_rest, None, duration))
            pending_rest = 0
            continue
        note = pitch_to_midi(pitch)
        if note is None:
            pending_rest += duration
            continue
        events.append((pending_rest, max(0, min(127, int(note))), duration))
        pending_rest = 0
    if pending_rest:
        events.append((pending_rest, None, 0))
    return events


def tone_events(
    tone_data: bytes,
    pitch_to_midi: Callable[[int], int | None] = midi_note_from_nokia_pitch,
) -> list[tuple[int, int | None, int]]:
    """Return trace-faithful (delta_ticks, midi_note_or_none, duration_ticks) events."""
    return _map_tone_events(tone_raw_events(tone_data), pitch_to_midi)


def tone_preview_events(
    tone_data: bytes,
    pitch_to_midi: Callable[[int], int | None] = midi_note_from_nokia_pitch,
    repeat_cycles: int = 1,
) -> list[tuple[int, int | None, int]]:
    """Return events for browser/macOS previews, trimming only terminal silence."""
    return _map_tone_events(tone_preview_raw_events(tone_data, repeat_cycles), pitch_to_midi)


def write_midi(
    path: Path,
    tone_data: bytes,
    pitch_to_midi: Callable[[int], int | None] = midi_note_from_nokia_pitch,
    repeat_cycles: int = 1,
) -> int:
    events = tone_preview_events(tone_data, pitch_to_midi, repeat_cycles)
    track = bytearray()
    track += b"\x00\xff\x51\x03" + MIDI_TEMPO_US.to_bytes(3, "big")
    track += b"\x00\xff\x03\x0aNokia tone"
    track += b"\x00\xc0\x50"  # lead square

    written_notes = 0
    trailing_rest = 0
    for delta, note, duration in events:
        if note is None:
            trailing_rest += delta + duration
            continue
        track += midi_vlq(trailing_rest + delta)
        trailing_rest = 0
        track += bytes([0x90, note, 96])
        track += midi_vlq(duration)
        track += bytes([0x80, note, 0])
        written_notes += 1

    track += midi_vlq(trailing_rest)
    track += b"\xff\x2f\x00"
    midi = b"MThd" + struct.pack(">IHHH", 6, 0, 1, MIDI_TPQ)
    midi += b"MTrk" + struct.pack(">I", len(track)) + bytes(track)
    path.write_bytes(midi)
    return written_notes


def midi_note_hz(note: int) -> float:
    return 440.0 * (2 ** ((note - 69) / 12))


def write_wav(
    path: Path,
    tone_data: bytes,
    pitch_to_hz: Callable[[int], float | None] | None = None,
    pitch_to_midi: Callable[[int], int | None] = midi_note_from_nokia_pitch,
    repeat_cycles: int = 1,
) -> int:
    pcm = bytearray()
    written_notes = 0
    raw_events = tone_preview_raw_events(tone_data, repeat_cycles)

    def resolve_hz(pitch: int) -> float | None:
        hz = pitch_to_hz(pitch) if pitch_to_hz else None
        if hz is not None:
            return hz
        note = pitch_to_midi(pitch)
        if note is None:
            return None
        return midi_note_hz(note)

    def append_silence(ticks: int) -> None:
        samples = max(0, round(ticks / MIDI_TICKS_PER_SECOND * WAV_SAMPLE_RATE))
        pcm.extend(b"\x00\x00" * samples)

    def append_speaker_hz(hz: float, ticks: int) -> None:
        nonlocal written_notes
        samples = max(1, round(ticks / MIDI_TICKS_PER_SECOND * WAV_SAMPLE_RATE))
        attack = min(samples // 2, round(WAV_SAMPLE_RATE * 0.0025))
        release = min(samples // 2, round(WAV_SAMPLE_RATE * 0.006))
        phase_step = 2.0 * math.pi * hz / WAV_SAMPLE_RATE
        prev = 0.0
        for i in range(samples):
            phase = phase_step * i
            # The 3210 uses its loudspeaker for these sounds. Keep the preview
            # mostly tonal and only lightly colored; the ASIC/gain model is not
            # decoded enough to justify a harsh square-wave renderer.
            sample = (
                0.86 * math.sin(phase)
                + 0.18 * math.sin(phase * 2.0 + 0.35)
                + 0.08 * math.sin(phase * 3.0)
            )
            sample = math.tanh(sample * 1.35) / math.tanh(1.35)
            # Tiny smoothing mimics the speaker path enough to remove the
            # brittle zipper edge without hiding Nokia's sharp note starts.
            sample = prev * 0.18 + sample * 0.82
            prev = sample
            gain = 1.0
            if attack:
                gain = min(gain, i / attack)
            if release:
                gain = min(gain, (samples - 1 - i) / release)
            pcm.extend(struct.pack("<h", round(sample * WAV_AMPLITUDE * max(0.0, gain))))
        written_notes += 1

    if not any(pitch is not None and resolve_hz(pitch) is not None for _delta, pitch, _duration in raw_events):
        with wave.open(str(path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(WAV_SAMPLE_RATE)
            wav.writeframes(b"")
        return 0

    for delta, pitch, duration in raw_events:
        if delta:
            append_silence(delta)
        if pitch is None:
            continue
        hz = resolve_hz(pitch)
        if hz is None:
            append_silence(duration)
            continue
        append_speaker_hz(hz, duration)

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(WAV_SAMPLE_RATE)
        wav.writeframes(bytes(pcm))

    return written_notes


def has_dynamic_engine_commands(tone_data: bytes) -> bool:
    start = tone_start_offset(tone_data)
    pos = start
    while pos < len(tone_data):
        cmd = tone_data[pos]
        if cmd == DYNAMIC_PITCH_COMMAND:
            return True
        if cmd == 0x0B:
            return False
        if cmd == 0x07 and pos + 1 < len(tone_data) and tone_data[pos + 1] == 0x0B:
            return False
        if cmd in {0x05, 0x0A} and pos + 1 < len(tone_data):
            pos += 2
            continue
        if cmd == 0x06:
            pos += 1
            continue
        if cmd in {0x09, 0x0C, 0x0D, 0x0F, 0x10}:
            pos += 1
            continue
        if cmd in {0x01, 0x02, 0x08, 0x0E} and pos + 1 < len(tone_data):
            pos += 2
            continue
        pos += 2
    return False


def export_tone(
    subchunk: dict[str, object],
    index: int,
    outdir: Path,
    pitch_to_midi: Callable[[int], int | None] = midi_note_from_nokia_pitch,
    pitch_to_hz: Callable[[int], float | None] | None = None,
    uses_mcu_note_frequency_table: bool = False,
) -> dict[str, object]:
    payload = subchunk["payload"]
    assert isinstance(payload, bytes)
    tone_id = clean_type(payload[8:12])
    flags = payload[12:16]
    data = payload[16:]
    name_len = data[0] if data else 0
    raw_name = data[1:max(1, name_len)]
    name = raw_name.rstrip(b"\x00").decode("latin1", "replace")
    tone_data = data[1 + name_len:] if name_len else b""

    stem = f"{index:02d}_{sanitize(name)}_{sanitize(tone_id)}"
    bin_path = outdir / "raw" / f"{stem}.bin"
    re_path = outdir / "re" / f"{stem}.re"
    tone_path = outdir / "tone_data" / f"{stem}.tone.bin"
    midi_path = outdir / "midi" / f"{stem}.mid"
    wav_path = outdir / "wav" / f"{stem}.wav"
    meta_path = outdir / "meta" / f"{stem}.json"

    bin_path.write_bytes(payload)
    re_payload = index.to_bytes(4, "big") + len(payload).to_bytes(4, "big") + payload[8:12] + flags + data
    re_path.write_bytes(re_payload)
    tone_path.write_bytes(tone_data)
    raw_events, end_offset, decode_status = tone_raw_events_with_end(tone_data)
    preview_repeat_cycles = 2 if decode_status in {"repeat_forever", "repeat_forever_truncated"} else 1
    raw_midi_events = _map_tone_events(raw_events, pitch_to_midi)
    preview_events = tone_preview_events(tone_data, pitch_to_midi, preview_repeat_cycles)
    raw_duration_seconds = (
        sum(delta + duration for delta, _note, duration in raw_midi_events) / MIDI_TICKS_PER_SECOND
        if raw_midi_events else 0.0
    )
    duration_seconds = (
        sum(delta + duration for delta, _note, duration in preview_events) / MIDI_TICKS_PER_SECOND
        if preview_events else 0.0
    )
    note_count = write_midi(midi_path, tone_data, pitch_to_midi, preview_repeat_cycles)
    wav_note_count = write_wav(wav_path, tone_data, pitch_to_hz, pitch_to_midi, preview_repeat_cycles)
    special_engine_stream = note_count == 0 and any(byte not in {0x00, 0x0B} for byte in tone_data[tone_start_offset(tone_data):])

    meta = {
        "index": index,
        "name": name,
        "id": tone_id,
        "ppm_numeric_id": subchunk["id"],
        "offset": subchunk["offset"],
        "length": subchunk["length"],
        "flags_hex": flags.hex(),
        "tone_data_size": len(tone_data),
        "tone_start_offset": tone_start_offset(tone_data),
        "tone_decode_end_offset": end_offset,
        "tone_decode_status": decode_status,
        "midi_note_count": note_count,
        "wav_note_count": wav_note_count,
        "wav_sample_rate": WAV_SAMPLE_RATE,
        "wav_renderer": WAV_RENDERER,
        "preview_repeat_cycles": preview_repeat_cycles,
        "wav_preview_trims_final_terminal_rest": True,
        "uses_mcu_note_frequency_table": uses_mcu_note_frequency_table,
        "special_engine_stream": special_engine_stream,
        "dynamic_pitch_command": has_dynamic_engine_commands(tone_data),
        "duration_seconds_estimate": round(duration_seconds, 3),
        "raw_duration_seconds_estimate": round(raw_duration_seconds, 3),
        "preview_includes_inter_pass_rest": preview_repeat_cycles > 1,
        "preview_final_terminal_rest_trimmed_seconds": round(
            max(0.0, raw_duration_seconds * preview_repeat_cycles - duration_seconds),
            3,
        ),
        "raw_sha256": sha256(payload),
        "startup_melody_ruled_out_by_hardware": True,
    }
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", type=Path)
    parser.add_argument("outdir", type=Path)
    args = parser.parse_args()

    data = args.firmware.read_bytes()
    ppm_start, chunks = parse_ppm(data)
    freq_table = firmware_note_frequency_table(data)
    pitch_to_midi = pitch_to_midi_from_frequency_table(freq_table)
    pitch_to_hz = pitch_to_hz_from_frequency_table(freq_table)

    chunks_dir = args.outdir / "ppm_chunks"
    tones_dir = args.outdir / "ringtones"
    for sub in ["raw", "re", "tone_data", "midi", "wav", "meta"]:
        (tones_dir / sub).mkdir(parents=True, exist_ok=True)
    chunks_dir.mkdir(parents=True, exist_ok=True)

    tone_manifest = []
    chunk_manifest = {
        "source": str(args.firmware),
        "source_sha256": sha256(data),
        "ppm_offset": ppm_start,
        "chunks": [],
    }

    for chunk in chunks:
        ctype = chunk["type"]
        cdir = chunks_dir / sanitize(str(ctype))
        cdir.mkdir(exist_ok=True)
        subrecords = []
        for i, subchunk in enumerate(chunk["subchunks"]):
            payload = subchunk["payload"]
            assert isinstance(payload, bytes)
            fname = f"{i:03d}_{sanitize(str(subchunk['type']))}_{int(subchunk['id']):08x}.bin"
            (cdir / fname).write_bytes(payload)
            subrecords.append(
                {
                    "index": i,
                    "id": subchunk["id"],
                    "type": subchunk["type"],
                    "offset": subchunk["offset"],
                    "length": subchunk["length"],
                    "file": str((cdir / fname).relative_to(args.outdir)),
                    "sha256": sha256(payload),
                }
            )
            if ctype == "TONE":
                tone_manifest.append(
                    export_tone(
                        subchunk,
                        i,
                        tones_dir,
                        pitch_to_midi,
                        pitch_to_hz,
                        uses_mcu_note_frequency_table=bool(freq_table),
                    )
                )

        chunk_manifest["chunks"].append(
            {
                "id": chunk["id"],
                "type": ctype,
                "offset": chunk["offset"],
                "length": chunk["length"],
                "subchunk_count": len(chunk["subchunks"]),
                "subchunks": subrecords,
            }
        )

    (args.outdir / "ppm_chunks_manifest.json").write_text(json.dumps(chunk_manifest, indent=2) + "\n")
    (tones_dir / "manifest.json").write_text(json.dumps({
        "startup_melody_status": PPM_TONE_STARTUP_MELODY_STATUS,
        "uses_mcu_note_frequency_table": bool(freq_table),
        "note_frequency_table": NOTE_FREQUENCY_TABLE if freq_table else None,
        "tones": tone_manifest,
    }, indent=2) + "\n")
    print(f"dumped {len(chunks)} PPM chunks and {len(tone_manifest)} ringtones")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
