#!/usr/bin/env python3
"""Extract MCU-resident Nokia 3210 system tones.

These are separate from the PPM TONE ringtone resources. The firmware's
play_tone() routine uses a 33-entry table of short system/warning/UI tones.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import wave
from pathlib import Path

from extract_ppm_chunks import (
    MIDI_TICKS_PER_SECOND,
    NORMAL_PITCH_MAX,
    TONE_UNIT_HZ,
    WAV_AMPLITUDE,
    sha256,
    sanitize,
    has_dynamic_engine_commands,
    tone_ticks,
    tone_events,
    tone_preview_events,
    tone_raw_events_with_end,
    tone_start_offset,
    write_midi,
    write_wav,
    WAV_RENDERER,
    WAV_SAMPLE_RATE,
)


BASE = 0x00200000
SYSTEM_TONE_TABLE = 0x002DC178
SYSTEM_TONE_COUNT = 33
PLAY_TONE = 0x002B1F24
NOTE_FREQUENCY_TABLE = 0x002D9FE8
TONE_COMMAND_04_HANDLER = 0x00297EA0
DTMF_HELPER = 0x002A1DF4
DTMF_PAIR_TABLE = 0x002E2A48
DTMF_NON_DTMF_FALLBACK_INDEX = 16

LABELS: dict[int, str] = {}

DYNAMIC_RUNTIME_PREVIEWS = {
    0: {
        "route": "generic_keypad_click",
        "mode_arg": 2,
        "source": "0x002a0bb0 posts 0x9b59(0, 2); 0x00277cf0 later calls play_tone(0, 2, 0).",
        "description": "Generic non-DTMF keypad click. Tone 00 uses command 0xa6, so pitch is the runtime mode byte; mode 2 indexes the firmware frequency table to 900 Hz.",
    },
}

RUNTIME_KEYED_DTMF_TEMPLATES = {
    1: {
        "route_status": "no exact v6.00 playback route pinned",
        "description": "Short runtime-keyed DTMF template. The 0xff operand means the DTMF key comes from the runtime mode byte.",
    },
    2: {
        "route_status": "only stop/control callsites are pinned: 0x002a0c64 and 0x002a0cb6",
        "description": "Longer runtime-keyed DTMF template. Current v6.00 trace uses it as a keyed-tone control/clear surface, not the generic keypad click.",
    },
    3: {
        "route_status": "actual standby/call/keyguard DTMF stream; runtime key is staged through 0x00277cb4 and replayed at 0x00277cf0",
        "description": "Runtime-keyed DTMF stream used for digit/star/hash tones. The 0xff operand means the audible DTMF key comes from the runtime mode byte.",
        "preview_policy": "Leave the static WAV empty; simulator playback synthesizes the per-key DTMF pair from the runtime ASCII key and the firmware DTMF table.",
    },
}

WELCOME_SOUND_TRACE = {
    "status": "system_tone_table_ruled_out",
    "ruled_out": [
        {
            "tone_index": 0,
            "reason": "Hardware comparison: user recognized it as a system sound, not the startup melody.",
        },
        {
            "tone_indexes": list(range(SYSTEM_TONE_COUNT)),
            "reason": "Hardware comparison: user checked the extracted MCU system-tone previews and did not find the startup melody.",
        }
    ],
    "evidence": [
        {
            "address": 0x002AA79C,
            "address_hex": "0x002aa79c",
            "description": "Original 5E0 dispatcher-table welcome handler. It posts UI message 0x5de during the welcome flow.",
        },
        {
            "address": 0x00299F94,
            "address_hex": "0x00299f94",
            "description": "A welcome-related handler posts UI message 0x5de after clearing screen elements 16, 17, and 18.",
        },
        {
            "address": 0x00299F9C,
            "address_hex": "0x00299f9c",
            "description": "Immediately after posting 0x5de, firmware checks RAM byte 0x00110f1f for values 23 and 24; hardware comparison now indicates this is not the startup melody path.",
        },
        {
            "address": 0x0029A2A0,
            "address_hex": "0x0029a2a0",
            "description": "UI message 0x5de handler reads RAM byte 0x00110f1f into r4.",
        },
        {
            "address": 0x0029A328,
            "address_hex": "0x0029a328",
            "description": "The 0x5de handler calls play_tone(0, 0xf1, r4) when r4 is non-zero.",
        },
    ],
    "high_priority_candidates": [],
    "secondary_candidates": [
        {
            "tone_index": 19,
            "reason": "The 0x5de handler can force r4=19 on one branch before play_tone(), but the whole MCU system-tone table is currently ruled out for startup melody.",
        },
        {
            "tone_index": 20,
            "reason": "The 0x5de handler can force r4=20 on another branch before play_tone(), but the whole MCU system-tone table is currently ruled out for startup melody.",
        },
    ],
    "next_search_area": "Orphan/non-menu melody bytecode and direct play_tone(pointer, ...) call sites outside the extracted PPM ringtone list.",
}


def addr_to_off(addr: int) -> int:
    return addr - BASE


def tone_stream_length(data: bytes, offset: int, max_len: int = 512) -> tuple[int, str]:
    view = data[offset:offset + max_len]
    _events, end, status = tone_raw_events_with_end(view, tone_start_offset(view))
    return min(end, len(view)), status


def note_frequency_table(data: bytes) -> dict[int, int]:
    table_off = addr_to_off(NOTE_FREQUENCY_TABLE)
    out: dict[int, int] = {}
    for pitch in range(0x40, NORMAL_PITCH_MAX + 1):
        off = table_off + 2 * (pitch - 0x40)
        if off + 2 > len(data):
            break
        freq = int.from_bytes(data[off:off + 2], "big")
        if freq > 0:
            out[pitch] = freq
    return out


def pitch_to_midi_from_table(table: dict[int, int]):
    def mapper(pitch: int) -> int | None:
        freq = table.get(pitch)
        if freq:
            return max(0, min(127, round(69 + 12 * math.log2(freq / 440.0))))
        if 0x7B <= pitch <= NORMAL_PITCH_MAX:
            return max(0, min(127, pitch - 54))
        if 0x41 <= pitch <= NORMAL_PITCH_MAX:
            return max(0, min(127, pitch - 54))
        return None

    return mapper


def pitch_to_hz_from_table(table: dict[int, int]):
    def mapper(pitch: int) -> float | None:
        freq = table.get(pitch)
        if freq:
            return float(freq)
        midi = pitch_to_midi_from_table(table)(pitch)
        if midi is None:
            return None
        return 440.0 * (2 ** ((midi - 69) / 12))

    return mapper


def runtime_pitch_frequency(data: bytes, mode_arg: int) -> int:
    off = addr_to_off(NOTE_FREQUENCY_TABLE) + mode_arg * 2
    if off < 0 or off + 2 > len(data):
        return 0
    return int.from_bytes(data[off:off + 2], "big")


def dynamic_runtime_command(payload: bytes, start_offset: int) -> dict[str, int] | None:
    if start_offset + 1 >= len(payload):
        return None
    if payload[start_offset] != 0xA6:
        return None
    return {
        "offset": start_offset,
        "duration_units": payload[start_offset + 1],
    }


def keyed_dtmf_command(payload: bytes) -> dict[str, int] | None:
    for offset in range(0, max(0, len(payload) - 2)):
        if payload[offset] == 0x04 and payload[offset + 1] == 0xFF:
            return {
                "offset": offset,
                "operand": payload[offset + 1],
                "duration_units": payload[offset + 2],
            }
    return None


def dtmf_pair_row(data: bytes, index: int) -> dict[str, object]:
    row = DTMF_PAIR_TABLE + index * 4
    first = int.from_bytes(data[addr_to_off(row):addr_to_off(row) + 2], "big")
    second = int.from_bytes(data[addr_to_off(row) + 2:addr_to_off(row) + 4], "big")
    return {
        "index": index,
        "address": f"0x{row:08x}",
        "first_frequency_word": first,
        "second_frequency_word": second,
        "raw_hex": data[addr_to_off(row):addr_to_off(row) + 4].hex(),
    }


def write_runtime_pitch_preview_wav(path: Path, frequency_hz: float, duration_units: int) -> int:
    duration = max(1, duration_units) / TONE_UNIT_HZ
    samples = max(1, round(duration * WAV_SAMPLE_RATE))
    attack = max(1, round(0.0025 * WAV_SAMPLE_RATE))
    release = max(1, round(0.012 * WAV_SAMPLE_RATE))
    pcm = bytearray()
    for i in range(samples):
        t = i / WAV_SAMPLE_RATE
        sample = math.sin(2.0 * math.pi * frequency_hz * t)
        envelope = math.exp(-1.7 * i / samples)
        if i < attack:
            envelope *= i / attack
        if i >= samples - release:
            envelope *= max(0.0, (samples - i - 1) / release)
        pcm.extend(struct.pack("<h", round(sample * WAV_AMPLITUDE * envelope)))
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(WAV_SAMPLE_RATE)
        wav.writeframes(bytes(pcm))
    return 1


def export_system_tone(
    data: bytes,
    index: int,
    ptr: int,
    flags: bytes,
    outdir: Path,
    max_len: int = 512,
) -> dict[str, object]:
    label = LABELS.get(index, "system")
    stem = f"{index:02d}_{sanitize(label)}"
    off = addr_to_off(ptr)
    length, decode_status = tone_stream_length(data, off, max_len)
    payload = data[off:off + length]

    raw_path = outdir / "raw" / f"{stem}.bin"
    midi_path = outdir / "midi" / f"{stem}.mid"
    wav_path = outdir / "wav" / f"{stem}.wav"
    meta_path = outdir / "meta" / f"{stem}.json"

    raw_path.write_bytes(payload)
    freq_table = note_frequency_table(data)
    pitch_to_midi = pitch_to_midi_from_table(freq_table)
    pitch_to_hz = pitch_to_hz_from_table(freq_table)
    midi_note_count = write_midi(midi_path, payload, pitch_to_midi)
    wav_note_count = write_wav(wav_path, payload, pitch_to_hz, pitch_to_midi)
    raw_duration_ticks = sum(delta + duration for delta, _note, duration in tone_events(payload, pitch_to_midi))
    preview_duration_ticks = sum(delta + duration for delta, _note, duration in tone_preview_events(payload, pitch_to_midi))
    start_offset = tone_start_offset(payload)
    dynamic_pitch_command = has_dynamic_engine_commands(payload)
    wav_renderer = WAV_RENDERER
    special_engine_stream = (
        dynamic_pitch_command
        or (midi_note_count == 0 and any(byte not in {0x00, 0x0B} for byte in payload[start_offset:]))
    )
    runtime_dynamic_preview = None
    runtime_keyed_dtmf_template = None
    runtime_preview = DYNAMIC_RUNTIME_PREVIEWS.get(index)
    dynamic_command = dynamic_runtime_command(payload, start_offset)
    if runtime_preview and dynamic_command:
        mode_arg = int(runtime_preview["mode_arg"])
        frequency_word = runtime_pitch_frequency(data, mode_arg)
        duration_units = dynamic_command["duration_units"]
        if frequency_word > 0 and duration_units > 0:
            wav_note_count = write_runtime_pitch_preview_wav(wav_path, frequency_word, duration_units)
            preview_duration_ticks = tone_ticks(duration_units)
            wav_renderer = "firmware_runtime_dynamic_pitch_preview"
            runtime_dynamic_preview = {
                **runtime_preview,
                "renderer": wav_renderer,
                "frequency_word": frequency_word,
                "frequency_hz": float(frequency_word),
                "duration_units": duration_units,
                "duration_seconds": round(duration_units / TONE_UNIT_HZ, 4),
                "command_offset": dynamic_command["offset"],
                "command_hex": "0xa6",
            }

    keyed_template = RUNTIME_KEYED_DTMF_TEMPLATES.get(index)
    keyed_command = keyed_dtmf_command(payload)
    if keyed_template and keyed_command:
        wav_renderer = "runtime_keyed_dtmf_template_unrendered"
        runtime_keyed_dtmf_template = {
            **keyed_template,
            "renderer": wav_renderer,
            "handler": f"0x{TONE_COMMAND_04_HANDLER:08x}",
            "dtmf_helper": f"0x{DTMF_HELPER:08x}",
            "command_offset": keyed_command["offset"],
            "command_hex": "0x04",
            "runtime_key_operand_hex": f"0x{keyed_command['operand']:02x}",
            "runtime_key_source": "tone slot byte +0x12",
            "duration_units": keyed_command["duration_units"],
            "duration_seconds": round(keyed_command["duration_units"] / TONE_UNIT_HZ, 4),
            "non_dtmf_fallback_row": dtmf_pair_row(data, DTMF_NON_DTMF_FALLBACK_INDEX),
            "preview_policy": keyed_template.get(
                "preview_policy",
                "Leave WAV empty unless a traced route supplies an actual runtime DTMF key; a guessed key would be misleading.",
            ),
        }

    meta = {
        "index": index,
        "label": label,
        "address": ptr,
        "offset": off,
        "length": length,
        "max_length_from_next_pointer": max_len,
        "flags_hex": flags.hex(),
        "start_offset": start_offset,
        "decode_status": decode_status,
        "special_engine_stream": special_engine_stream,
        "dynamic_pitch_command": dynamic_pitch_command,
        "midi_note_count": midi_note_count,
        "wav_note_count": wav_note_count,
        "wav_sample_rate": WAV_SAMPLE_RATE,
        "wav_renderer": wav_renderer,
        "preview_repeat_cycles": 1,
        "wav_preview_trims_final_terminal_rest": True,
        "duration_ticks_estimate": preview_duration_ticks,
        "raw_duration_ticks_estimate": raw_duration_ticks,
        "preview_final_terminal_rest_trimmed_ticks": max(0, raw_duration_ticks - preview_duration_ticks),
        "duration_seconds_estimate": round(preview_duration_ticks / MIDI_TICKS_PER_SECOND, 3),
        "special_reference_preview": None,
        "uses_mcu_note_frequency_table": True,
        "raw": str(raw_path.relative_to(outdir.parent)),
        "midi": str(midi_path.relative_to(outdir.parent)),
        "wav": str(wav_path.relative_to(outdir.parent)),
        "raw_sha256": sha256(payload),
    }
    if runtime_dynamic_preview is not None:
        meta["runtime_dynamic_preview"] = runtime_dynamic_preview
    if runtime_keyed_dtmf_template is not None:
        meta["runtime_keyed_dtmf_template"] = runtime_keyed_dtmf_template
    meta_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return meta


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", type=Path)
    parser.add_argument("outdir", type=Path)
    args = parser.parse_args()

    data = args.firmware.read_bytes()
    outdir = args.outdir / "system_tones"
    for sub in ["raw", "midi", "wav", "meta"]:
        subdir = outdir / sub
        subdir.mkdir(parents=True, exist_ok=True)
        for old in subdir.iterdir():
            if old.is_file():
                old.unlink()

    tones = []
    table_off = addr_to_off(SYSTEM_TONE_TABLE)
    pointers = []
    for index in range(SYSTEM_TONE_COUNT):
        entry = data[table_off + index * 8:table_off + index * 8 + 8]
        ptr = int.from_bytes(entry[:4], "big")
        if ptr:
            pointers.append(ptr)

    for index in range(SYSTEM_TONE_COUNT):
        entry = data[table_off + index * 8:table_off + index * 8 + 8]
        ptr = int.from_bytes(entry[:4], "big")
        flags = entry[4:]
        if ptr == 0:
            continue
        next_ptr = min((candidate for candidate in pointers if candidate > ptr), default=ptr + 512)
        tones.append(export_system_tone(data, index, ptr, flags, outdir, next_ptr - ptr))

    manifest = {
        "source": str(args.firmware),
        "addressing": "ARM big-endian Thumb, flash base 0x00200000",
        "play_tone": PLAY_TONE,
        "system_tone_table": SYSTEM_TONE_TABLE,
        "tone_count": len(tones),
        "welcome_sound_trace": WELCOME_SOUND_TRACE,
        "notes": [
            "System tones are MCU-resident and separate from PPM ringtone TONE resources.",
            "No system tone is currently marked as the startup melody; hardware comparison ruled out the MCU system-tone table for this sound.",
            "The earlier welcome-sound trace reaches UI message 0x5de, where play_tone() uses a RAM-selected system tone. That trace is now kept as related UI/profile evidence, not as the boot melody.",
            "MIDI/WAV exports are approximate previews; raw .bin streams are the preservation-grade artifacts.",
            "Some entries are special engine streams rather than normal note bytecode; these are marked with special_engine_stream in metadata.",
        ],
        "tones": tones,
    }
    (outdir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (outdir / "welcome_sound_trace.json").write_text(json.dumps(WELCOME_SOUND_TRACE, indent=2) + "\n", encoding="utf-8")
    print(f"extracted {len(tones)} MCU system tones")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
