#!/usr/bin/env python3
import json
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LEGACY_NAME = "gubby.ogg"
CANONICAL_NAME = "heartkate-helloktty_gubby-this-gubby-that.ogg"
TRACK_TIME_MS = 87031
DURATION_TOLERANCE_MS = 1500

METADATA = {
    "title": "gubby this gubby that",
    "artist": "heartkate & helloktty",
    "album": "gubby this gubby that - Single",
    "album_artist": "heartkate & helloktty",
    "date": "2026-05-11",
    "genre": "Alternative",
    "track": "1/1",
    "disc": "1/1",
    "label": "11659041 Records DK",
    "itunes_track_id": "6771541158",
    "itunes_album_id": "6771541157",
    "comment": "Metadata matched against Apple Music/iTunes catalog track 6771541158.",
}


def find_tool(name):
    for candidate in (name, f"{name}.exe"):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise RuntimeError(f"{name} was not found on PATH")


def probe_format(path, entries):
    result = subprocess.run(
        [
            find_tool("ffprobe"),
            "-v",
            "error",
            "-show_entries",
            entries,
            "-of",
            "json",
            str(path),
        ],
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return json.loads(result.stdout)


def probe_duration_ms(path):
    payload = probe_format(path, "format=duration")
    return round(float(payload["format"]["duration"]) * 1000)


def probe_stream_tags(path):
    payload = probe_format(path, "stream_tags")
    streams = payload.get("streams", [])
    if not streams:
        return {}
    return {key.lower(): value for key, value in streams[0].get("tags", {}).items()}


def tags_are_current(path):
    tags = probe_stream_tags(path)
    for key, value in METADATA.items():
        if tags.get(key.lower()) != value:
            return False
    return True


def source_path():
    canonical = ROOT / CANONICAL_NAME
    legacy = ROOT / LEGACY_NAME
    if canonical.exists():
        return canonical
    if legacy.exists():
        return legacy
    raise FileNotFoundError(f"Neither {CANONICAL_NAME} nor {LEGACY_NAME} exists in {ROOT}")


def tag_and_rename(source):
    duration_ms = probe_duration_ms(source)
    delta_ms = abs(duration_ms - TRACK_TIME_MS)
    if delta_ms > DURATION_TOLERANCE_MS:
        raise RuntimeError(
            f"{source.name} is {duration_ms} ms, expected about {TRACK_TIME_MS} ms "
            f"for '{METADATA['title']}'. Refusing to tag the wrong gubby."
        )

    target = ROOT / CANONICAL_NAME
    if source == target and tags_are_current(source):
        return target, duration_ms, delta_ms, False

    temp = ROOT / f".{CANONICAL_NAME}.tmp"
    if temp.exists():
        temp.unlink()

    command = [
        find_tool("ffmpeg"),
        "-y",
        "-i",
        str(source),
        "-map",
        "0:a:0",
        "-map_metadata",
        "-1",
        "-c:a",
        "copy",
    ]
    for key, value in METADATA.items():
        command.extend(["-metadata", f"{key}={value}"])
    command.extend(["-f", "ogg", str(temp)])

    try:
        subprocess.run(command, cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as exc:
        message = exc.stderr.decode("utf-8", errors="replace") if exc.stderr else str(exc)
        raise RuntimeError(f"ffmpeg failed while tagging {source.name}:\n{message}") from exc
    temp.replace(target)
    if source != target and source.exists():
        source.unlink()

    return target, duration_ms, delta_ms, True


def main():
    try:
        target, duration_ms, delta_ms, changed = tag_and_rename(source_path())
    except Exception as exc:
        print(f"gubby metadata fix failed: {exc}", file=sys.stderr)
        return 1

    action = "Tagged" if changed else "Already tagged"
    print(f"{action} {target.name}")
    print(f"Duration: {duration_ms} ms; Apple catalog delta: {delta_ms} ms")
    print(f"Title: {METADATA['title']}")
    print(f"Artist: {METADATA['artist']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
