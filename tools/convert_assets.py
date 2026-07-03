#!/usr/bin/env python3
import argparse
import json
import math
import shutil
import subprocess
import wave
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "resources" / "generated"
HEADER = ROOT / "src" / "c" / "generated_assets.h"
PACKAGE = ROOT / "package.json"

SCREEN_W = 200
SCREEN_H = 228
PAN_W = 220
AUDIO_RATE = 8000
AUDIO_SOURCE_RATE = 8000
AUDIO_UPSAMPLE = AUDIO_RATE // AUDIO_SOURCE_RATE
AUDIO_CHUNK_BYTES = 60 * 1024

PHOTO_FILES = [
    "bigGabba.jpg",
    "gabagoolandmoremeats.jpg",
    "gabagoolandtomatos.jpg",
    "gabagoolmountain.jpg",
    "gabagoolothersandwitch.jpg",
    "gabagoolsandwitch.jpg",
    "TonyGabbagool.jpg",
]

PEBBLE_LEVELS = [0x00, 0x55, 0xAA, 0xFF]
PEBBLE_PALETTE = [(r, g, b) for r in PEBBLE_LEVELS for g in PEBBLE_LEVELS for b in PEBBLE_LEVELS]
STEP_TABLE_2BIT = [
    2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 19, 22, 26, 31, 36,
    42, 49, 57, 66, 77, 89, 103, 119, 127,
]


def nearest_palette_color(pixel):
    r, g, b = pixel
    best = PEBBLE_PALETTE[0]
    best_dist = 1 << 62
    for pr, pg, pb in PEBBLE_PALETTE:
        dr = r - pr
        dg = g - pg
        db = b - pb
        dist = dr * dr + dg * dg + db * db
        if dist < best_dist:
            best_dist = dist
            best = (pr, pg, pb)
    return best


def dither_to_pebble_palette(image):
    width, height = image.size
    pixels = [[[float(c) for c in image.getpixel((x, y))[:3]] for x in range(width)] for y in range(height)]
    out = Image.new("RGB", (width, height))

    for y in range(height):
        for x in range(width):
            old = pixels[y][x]
            new = nearest_palette_color(tuple(max(0, min(255, int(round(c)))) for c in old))
            out.putpixel((x, y), new)
            err = [old[i] - new[i] for i in range(3)]
            # Atkinson dithering error diffusion to 6 neighbors
            for dx, dy in ((1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)):
                nx = x + dx
                ny = y + dy
                if 0 <= nx < width and 0 <= ny < height:
                    for i in range(3):
                        pixels[ny][nx][i] += err[i] * 0.125
    return out


def crop_cover(image, target_w, target_h):
    src_w, src_h = image.size
    scale = max(target_w / src_w, target_h / src_h)
    resized = image.resize((math.ceil(src_w * scale), math.ceil(src_h * scale)), Image.Resampling.LANCZOS)
    left = (resized.width - target_w) // 2
    top = (resized.height - target_h) // 2
    return resized.crop((left, top, left + target_w, top + target_h))


def write_palette_png(image, path):
    palette = []
    for r, g, b in PEBBLE_PALETTE:
        palette.extend([r, g, b])
    palette.extend([0] * (768 - len(palette)))
    
    palette_img = Image.new("P", (1, 1))
    palette_img.putpalette(palette)
    
    paletted = image.quantize(palette=palette_img, dither=Image.Dither.NONE)
    paletted.save(path, optimize=True)


def convert_images():
    image_entries = []
    for index, name in enumerate(PHOTO_FILES):
        source = ROOT / name
        if not source.exists():
            raise FileNotFoundError(source)
        image = Image.open(source).convert("RGB")
        strip = crop_cover(image, PAN_W, SCREEN_H)
        dithered = dither_to_pebble_palette(strip)
        out_path = GENERATED / f"photo_{index:02d}.png"
        write_palette_png(dithered, out_path)
        image_entries.append({
            "type": "bitmap",
            "name": f"IMAGE_{index:02d}",
            "file": f"generated/photo_{index:02d}.png",
            "memoryFormat": "8Bit"
        })

    icon_source = Image.open(ROOT / PHOTO_FILES[0]).convert("RGB")
    icon = dither_to_pebble_palette(crop_cover(icon_source, 25, 25))
    icon_path = GENERATED / "menu_icon.png"
    write_palette_png(icon, icon_path)
    icon_entry = {
        "type": "bitmap",
        "name": "MENU_ICON",
        "file": "generated/menu_icon.png",
        "menuIcon": True,
        "memoryFormat": "8Bit"
    }
    return icon_entry, image_entries


def ffmpeg_command():
    for candidate in ("ffmpeg", "ffmpeg.exe"):
        if shutil.which(candidate):
            return candidate
    raise RuntimeError("ffmpeg was not found on PATH")


def decode_ogg_to_pcm(source_file):
    wav_path = GENERATED / f"_{source_file.stem}_8k.wav"
    try:
        subprocess.run([
            ffmpeg_command(), "-y", "-i", source_file.name,
            "-ac", "1", "-ar", str(AUDIO_SOURCE_RATE), "-sample_fmt", "s16",
            str(wav_path.relative_to(ROOT))
        ], cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        with wave.open(str(wav_path), "rb") as wav:
            if wav.getnchannels() != 1 or wav.getframerate() != AUDIO_SOURCE_RATE or wav.getsampwidth() != 2:
                raise RuntimeError("Unexpected decoded WAV format")
            frames = wav.readframes(wav.getnframes())
    except subprocess.CalledProcessError as exc:
        message = exc.stderr.decode("utf-8", errors="replace") if exc.stderr else str(exc)
        raise RuntimeError(f"ffmpeg failed while decoding {source_file.name}:\n{message}") from exc
    finally:
        wav_path.unlink(missing_ok=True)

    pcm = []
    for i in range(0, len(frames), 2):
        sample = int.from_bytes(frames[i:i + 2], "little", signed=True)
        pcm.append(max(-128, min(127, sample // 256)))
    return pcm


def encode_2bit_adpcm(pcm):
    predictor = 0
    step_index = 10
    packed = bytearray()
    out_byte = 0
    shift = 0

    for sample in pcm:
        best_code = 0
        best_error = 1 << 30
        best_predictor = predictor
        best_index = step_index

        for code in range(4):
            trial_pred, trial_index = decode_code_2bit(predictor, step_index, code)
            error = abs(sample - trial_pred)
            if error < best_error:
                best_error = error
                best_code = code
                best_predictor = trial_pred
                best_index = trial_index

        out_byte |= best_code << shift
        predictor = best_predictor
        step_index = best_index
        shift += 2
        if shift == 8:
            packed.append(out_byte)
            out_byte = 0
            shift = 0

    if shift:
        packed.append(out_byte)

    return packed


def decode_code_2bit(predictor, step_index, code):
    step = STEP_TABLE_2BIT[step_index]
    diff = step // 2
    if code & 1:
        diff += step
    if code & 2:
        predictor -= diff
    else:
        predictor += diff
    predictor = max(-128, min(127, predictor))
    step_index += 2 if (code & 1) else -1
    step_index = max(0, min(len(STEP_TABLE_2BIT) - 1, step_index))
    return predictor, step_index


def convert_audio():
    ogg_files = sorted(list(ROOT.glob("*.ogg")))
    if not ogg_files:
        raise FileNotFoundError("No .ogg files found in the root directory")

    entries = []
    tracks_info = []

    for track_idx, ogg_file in enumerate(ogg_files):
        pcm = decode_ogg_to_pcm(ogg_file)
        
        # Normalize PCM to use full 8-bit dynamic range
        max_val = max(abs(s) for s in pcm)
        if max_val > 0:
            scale = 127.0 / max_val
            pcm = [max(-128, min(127, int(round(s * scale)))) for s in pcm]
            
        encoded = encode_2bit_adpcm(pcm)

        chunk_count = math.ceil(len(encoded) / AUDIO_CHUNK_BYTES)
        track_chunk_names = []
        for chunk_idx in range(chunk_count):
            chunk = encoded[chunk_idx * AUDIO_CHUNK_BYTES:(chunk_idx + 1) * AUDIO_CHUNK_BYTES]
            name = f"AUDIO_{track_idx:02d}_{chunk_idx:02d}"
            out_path = GENERATED / f"audio_{track_idx:02d}_{chunk_idx:02d}.ad2"
            out_path.write_bytes(chunk)
            
            entries.append({
                "type": "raw",
                "name": name,
                "file": f"generated/audio_{track_idx:02d}_{chunk_idx:02d}.ad2"
            })
            track_chunk_names.append(name)
            
        tracks_info.append({
            "filename": ogg_file.name,
            "total_samples": len(pcm),
            "encoded_bytes": len(encoded),
            "chunk_count": chunk_count,
            "chunk_names": track_chunk_names
        })
        
    return entries, tracks_info


def update_package(icon_entry, image_entries, audio_entries):
    package = json.loads(PACKAGE.read_text())
    package["pebble"]["resources"]["media"] = [icon_entry] + image_entries + audio_entries
    PACKAGE.write_text(json.dumps(package, indent=2) + "\n")


def write_header(image_entries, tracks_info):
    image_ids = ", ".join(f"RESOURCE_ID_{entry['name']}" for entry in image_entries)
    
    track_definitions = ""
    track_initializers = []
    
    for idx, track in enumerate(tracks_info):
        chunk_ids = ", ".join(f"RESOURCE_ID_{name}" for name in track["chunk_names"])
        track_definitions += f"""// Track {idx}: {track['filename']}
#define GABAGOOL_TRACK_{idx}_CHUNKS {track['chunk_count']}
static const uint32_t GABAGOOL_TRACK_{idx}_RESOURCE_IDS[GABAGOOL_TRACK_{idx}_CHUNKS] = {{
  {chunk_ids}
}};

"""
        track_initializers.append(
            f"  {{ {track['total_samples']}u, {track['encoded_bytes']}u, GABAGOOL_TRACK_{idx}_CHUNKS, GABAGOOL_TRACK_{idx}_RESOURCE_IDS }}"
        )
        
    track_initializers_str = ",\n".join(track_initializers)
    
    HEADER.write_text(f"""#pragma once

#define GABAGOOL_SCREEN_W {SCREEN_W}
#define GABAGOOL_SCREEN_H {SCREEN_H}
#define GABAGOOL_PAN_W {PAN_W}
#define GABAGOOL_AUDIO_RATE {AUDIO_RATE}
#define GABAGOOL_AUDIO_SOURCE_RATE {AUDIO_SOURCE_RATE}
#define GABAGOOL_AUDIO_UPSAMPLE {AUDIO_UPSAMPLE}
#define GABAGOOL_IMAGE_COUNT {len(image_entries)}
#define GABAGOOL_AUDIO_TRACK_COUNT {len(tracks_info)}

static const uint32_t GABAGOOL_IMAGE_RESOURCE_IDS[GABAGOOL_IMAGE_COUNT] = {{
  {image_ids}
}};

{track_definitions}
typedef struct {{
  uint32_t total_samples;
  uint32_t encoded_bytes;
  uint32_t chunk_count;
  const uint32_t *chunk_resource_ids;
}} GabagoolAudioTrack;

static const GabagoolAudioTrack GABAGOOL_AUDIO_TRACKS[GABAGOOL_AUDIO_TRACK_COUNT] = {{
{track_initializers_str}
}};
""")


def main():
    parser = argparse.ArgumentParser(description="Generate dithered Pebble assets and packed watch audio.")
    parser.parse_args()

    GENERATED.mkdir(parents=True, exist_ok=True)
    icon_entry, image_entries = convert_images()
    audio_entries, tracks_info = convert_audio()
    update_package(icon_entry, image_entries, audio_entries)
    write_header(image_entries, tracks_info)
    
    total_samples = sum(t["total_samples"] for t in tracks_info)
    encoded_bytes = sum(t["encoded_bytes"] for t in tracks_info)
    print(f"Generated {len(image_entries)} dithered images and {len(audio_entries)} audio chunks.")
    print(f"Audio: {len(tracks_info)} tracks, {total_samples / AUDIO_SOURCE_RATE:.1f}s, {encoded_bytes} encoded bytes.")


if __name__ == "__main__":
    main()
