#!/usr/bin/env python3
import argparse
import hashlib
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
AUDIO_CACHE = GENERATED / "audio_cache.json"

SCREEN_W = 200
SCREEN_H = 228
PAN_W = 320
AUDIO_RATE = 8000
AUDIO_SOURCE_RATE = 8000
AUDIO_MAX_UPSAMPLE = math.ceil(AUDIO_RATE / AUDIO_SOURCE_RATE)
AUDIO_CODEC_BITS = 4
AUDIO_CHUNK_BYTES = 60 * 1024
AUDIO_CACHE_VERSION = 2
SFX_AUDIO_FILES = {"gubbysfx.ogg"}

PHOTO_FILES = [
    "welcome.png",
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
STEP_TABLE_4BIT = [
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 20, 23,
    27, 31, 36, 42, 49, 57, 66, 77, 89, 103, 119, 127,
]
INDEX_TABLE_4BIT = [-1, -1, -1, -1, 2, 4, 6, 8]


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


def crop_cover_background(image, target_h):
    src_w, src_h = image.size
    scale = target_h / src_h
    new_w = math.ceil(src_w * scale)
    if new_w > PAN_W:
        new_w = PAN_W
        scale = PAN_W / src_w
        if src_h * scale < target_h:
            scale = target_h / src_h
            new_w = PAN_W
    else:
        if new_w < 200:
            scale = 200 / src_w
            new_w = 200

    new_h = math.ceil(src_h * scale)
    resized = image.resize((math.ceil(src_w * scale), new_h), Image.Resampling.LANCZOS)
    left = (resized.width - new_w) // 2
    top = (resized.height - target_h) // 2
    return resized.crop((left, top, left + new_w, top + target_h))


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
    image_widths = []
    for index, name in enumerate(PHOTO_FILES):
        source = ROOT / name
        if not source.exists():
            raise FileNotFoundError(source)
        image = Image.open(source).convert("RGB")
        strip = crop_cover_background(image, SCREEN_H)
        dithered = dither_to_pebble_palette(strip)
        image_widths.append(dithered.width)
        out_path = GENERATED / f"photo_{index:02d}.png"
        write_palette_png(dithered, out_path)
        image_entries.append({
            "type": "bitmap",
            "name": f"IMAGE_{index:02d}",
            "file": f"generated/photo_{index:02d}.png",
            "memoryFormat": "8Bit"
        })

    icon_source = Image.open(ROOT / PHOTO_FILES[1]).convert("RGB")
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
    return icon_entry, image_entries, image_widths


def convert_frame_to_pebble_png8(frame):
    rgba = frame.convert("RGBA")
    w, h = rgba.size
    palette_data = []
    for r, g, b in PEBBLE_PALETTE:
        palette_data.extend([r, g, b])
    palette_data.extend([0] * (765 - len(palette_data)))
    palette_data.extend([0, 255, 0])
    out = Image.new("P", (w, h))
    out.putpalette(palette_data)
    for y in range(h):
        for x in range(w):
            r, g, b, a = rgba.getpixel((x, y))
            if a < 128:
                out.putpixel((x, y), 255)
            else:
                best_idx = 0
                best_dist = 1 << 30
                for idx, (pr, pg, pb) in enumerate(PEBBLE_PALETTE):
                    dist = (r - pr)**2 + (g - pg)**2 + (b - pb)**2
                    if dist < best_dist:
                        best_dist = dist
                        best_idx = idx
                out.putpixel((x, y), best_idx)
    return out


def convert_pet_assets():
    static_files = [
        "gubbyshapedegg.png",
        "eggtap1.png",
        "eggtap2.png",
        "eggtap3.png",
        "deadgubby.png"
    ]
    for name in static_files:
        src = ROOT / name
        if src.exists():
            shutil.copy2(src, GENERATED / name)
            
    gif_files = [
        ("gubby.gif", "gubby.png"),
        ("gubbypetted.gif", "gubbypetted.png"),
        ("gubbydeath.gif", "gubbydeath.png")
    ]
    for src_name, dst_name in gif_files:
        src = ROOT / src_name
        if src.exists():
            subprocess.run([
                "wsl", "/home/h3nry/.local/bin/gif2apng", "-z0", src_name, f"resources/generated/{dst_name}"
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            
    pet_entries = [
        {
            "type": "bitmap",
            "name": "PET_EGG",
            "file": "generated/gubbyshapedegg.png",
            "memoryFormat": "8Bit"
        },
        {
            "type": "bitmap",
            "name": "PET_EGG_TAP1",
            "file": "generated/eggtap1.png",
            "memoryFormat": "8Bit"
        },
        {
            "type": "bitmap",
            "name": "PET_EGG_TAP2",
            "file": "generated/eggtap2.png",
            "memoryFormat": "8Bit"
        },
        {
            "type": "bitmap",
            "name": "PET_EGG_TAP3",
            "file": "generated/eggtap3.png",
            "memoryFormat": "8Bit"
        },
        {
            "type": "bitmap",
            "name": "PET_DEAD",
            "file": "generated/deadgubby.png",
            "memoryFormat": "8Bit"
        },
        {
            "type": "raw",
            "name": "PET_GUBBY_ANIM",
            "file": "generated/gubby.png"
        },
        {
            "type": "raw",
            "name": "PET_PETTED_ANIM",
            "file": "generated/gubbypetted.png"
        },
        {
            "type": "raw",
            "name": "PET_DEATH_ANIM",
            "file": "generated/gubbydeath.png"
        }
    ]
    return pet_entries


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


def encode_4bit_adpcm(pcm):
    predictor = 0
    step_index = 8
    packed = bytearray()
    out_byte = 0
    shift = 0

    for sample in pcm:
        best_code = 0
        best_error = 1 << 30
        best_predictor = predictor
        best_index = step_index

        for code in range(16):
            trial_pred, trial_index = decode_code_4bit(predictor, step_index, code)
            error = abs(sample - trial_pred)
            if error < best_error:
                best_error = error
                best_code = code
                best_predictor = trial_pred
                best_index = trial_index

        out_byte |= best_code << shift
        predictor = best_predictor
        step_index = best_index
        shift += AUDIO_CODEC_BITS
        if shift == 8:
            packed.append(out_byte)
            out_byte = 0
            shift = 0

    if shift:
        packed.append(out_byte)

    return packed


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


def decode_code_4bit(predictor, step_index, code):
    step = STEP_TABLE_4BIT[step_index]
    magnitude = code & 0x7
    diff = step >> 3
    if magnitude & 1:
        diff += step >> 2
    if magnitude & 2:
        diff += step >> 1
    if magnitude & 4:
        diff += step
    if code & 8:
        predictor -= diff
    else:
        predictor += diff
    predictor = max(-128, min(127, predictor))
    step_index += INDEX_TABLE_4BIT[magnitude]
    step_index = max(0, min(len(STEP_TABLE_4BIT) - 1, step_index))
    return predictor, step_index


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


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def audio_cache_settings():
    return {
        "version": AUDIO_CACHE_VERSION,
        "source_rate": AUDIO_SOURCE_RATE,
        "rate": AUDIO_RATE,
        "max_upsample": AUDIO_MAX_UPSAMPLE,
        "chunk_bytes": AUDIO_CHUNK_BYTES,
        "codec_bits": AUDIO_CODEC_BITS,
        "encoder": "4bit-adpcm-v1",
    }


def load_audio_cache():
    if not AUDIO_CACHE.exists():
        return {"settings": audio_cache_settings(), "tracks": {}}
    try:
        return json.loads(AUDIO_CACHE.read_text())
    except json.JSONDecodeError:
        return {"settings": audio_cache_settings(), "tracks": {}}


def audio_cache_key(track_idx, filename):
    return f"{track_idx:02d}:{filename}"


def audio_role(filename):
    return "sfx" if filename.lower() in SFX_AUDIO_FILES else "music"


def audio_cache_entry_valid(cache, entry, source_hash):
    if cache.get("settings") != audio_cache_settings():
        return False
    if not entry or entry.get("sha256") != source_hash:
        return False
    if len(entry.get("chunk_files", [])) != entry.get("chunk_count"):
        return False
    if len(entry.get("chunk_sizes", [])) != entry.get("chunk_count"):
        return False
    if len(entry.get("chunk_names", [])) != entry.get("chunk_count"):
        return False
    for chunk_file, chunk_size in zip(entry.get("chunk_files", []), entry.get("chunk_sizes", [])):
        path = GENERATED / chunk_file
        if not path.exists() or path.stat().st_size != chunk_size:
            return False
    return True


def audio_entries_from_track(track):
    return [
        {
            "type": "raw",
            "name": name,
            "file": f"generated/{chunk_file}"
        }
        for name, chunk_file in zip(track["chunk_names"], track["chunk_files"])
    ]


def prune_stale_audio_chunks(cache_manifest):
    keep = {
        chunk_file
        for track in cache_manifest["tracks"].values()
        for chunk_file in track.get("chunk_files", [])
    }
    for path in list(GENERATED.glob("audio_*.ad2")) + list(GENERATED.glob("audio_*.ad4")):
        if path.name not in keep:
            path.unlink()


def convert_audio():
    ogg_files = sorted(path for path in ROOT.glob("*.ogg") if not path.name.startswith("."))
    if not ogg_files:
        raise FileNotFoundError("No .ogg files found in the root directory")

    entries = []
    tracks_info = []
    cache = load_audio_cache()
    next_cache = {"settings": audio_cache_settings(), "tracks": {}}
    cache_hits = 0

    for track_idx, ogg_file in enumerate(ogg_files):
        source_hash = file_sha256(ogg_file)
        key = audio_cache_key(track_idx, ogg_file.name)
        cached_track = cache.get("tracks", {}).get(key)
        if audio_cache_entry_valid(cache, cached_track, source_hash):
            role = audio_role(ogg_file.name)
            entries.extend(audio_entries_from_track(cached_track))
            tracks_info.append({
                "filename": cached_track["filename"],
                "total_samples": cached_track["total_samples"],
                "encoded_bytes": cached_track["encoded_bytes"],
                "chunk_count": cached_track["chunk_count"],
                "chunk_names": cached_track["chunk_names"],
                "role": role
            })
            next_cache["tracks"][key] = {**cached_track, "role": role}
            cache_hits += 1
            continue

        pcm = decode_ogg_to_pcm(ogg_file)
        
        # Normalize PCM to use full 8-bit dynamic range
        max_val = max(abs(s) for s in pcm)
        if max_val > 0:
            scale = 127.0 / max_val
            pcm = [max(-128, min(127, int(round(s * scale)))) for s in pcm]
            
        encoded = encode_4bit_adpcm(pcm)

        chunk_count = math.ceil(len(encoded) / AUDIO_CHUNK_BYTES)
        track_chunk_names = []
        track_chunk_files = []
        track_chunk_sizes = []
        for chunk_idx in range(chunk_count):
            chunk = encoded[chunk_idx * AUDIO_CHUNK_BYTES:(chunk_idx + 1) * AUDIO_CHUNK_BYTES]
            name = f"AUDIO_{track_idx:02d}_{chunk_idx:02d}"
            chunk_file = f"audio_{track_idx:02d}_{chunk_idx:02d}.ad4"
            out_path = GENERATED / chunk_file
            out_path.write_bytes(chunk)
            
            entries.append({
                "type": "raw",
                "name": name,
                "file": f"generated/{chunk_file}"
            })
            track_chunk_names.append(name)
            track_chunk_files.append(chunk_file)
            track_chunk_sizes.append(len(chunk))
            
        track_info = {
            "filename": ogg_file.name,
            "total_samples": len(pcm),
            "encoded_bytes": len(encoded),
            "chunk_count": chunk_count,
            "chunk_names": track_chunk_names,
            "role": audio_role(ogg_file.name)
        }
        tracks_info.append(track_info)
        next_cache["tracks"][key] = {
            **track_info,
            "sha256": source_hash,
            "chunk_files": track_chunk_files,
            "chunk_sizes": track_chunk_sizes
        }

    prune_stale_audio_chunks(next_cache)
    AUDIO_CACHE.write_text(json.dumps(next_cache, indent=2) + "\n")
    if cache_hits:
        print(f"Audio cache: reused {cache_hits} of {len(ogg_files)} tracks.")
        
    return entries, tracks_info


def update_package(icon_entry, image_entries, audio_entries, pet_entries):
    package = json.loads(PACKAGE.read_text())
    package["pebble"]["resources"]["media"] = [icon_entry] + image_entries + audio_entries + pet_entries
    PACKAGE.write_text(json.dumps(package, indent=2) + "\n")


def write_header(image_entries, image_widths, tracks_info):
    image_ids = ", ".join(f"RESOURCE_ID_{entry['name']}" for entry in image_entries)
    widths_str = ", ".join(str(w) for w in image_widths)
    
    track_definitions = ""
    track_initializers = []
    music_track_indexes = []
    gubby_sfx_track_index = -1
    
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
        if track.get("role") == "sfx":
            if track["filename"].lower() == "gubbysfx.ogg":
                gubby_sfx_track_index = idx
        else:
            music_track_indexes.append(idx)
        
    track_initializers_str = ",\n".join(track_initializers)
    music_track_indexes_str = ", ".join(f"{idx}u" for idx in music_track_indexes) or "0u"
    
    HEADER.write_text(f"""#pragma once

#define GABAGOOL_SCREEN_W {SCREEN_W}
#define GABAGOOL_SCREEN_H {SCREEN_H}
#define GABAGOOL_PAN_W {PAN_W}
#define GABAGOOL_AUDIO_RATE {AUDIO_RATE}
#define GABAGOOL_AUDIO_SOURCE_RATE {AUDIO_SOURCE_RATE}
#define GABAGOOL_AUDIO_MAX_UPSAMPLE {AUDIO_MAX_UPSAMPLE}
#define GABAGOOL_AUDIO_CODEC_BITS {AUDIO_CODEC_BITS}
#define GABAGOOL_IMAGE_COUNT {len(image_entries)}
#define GABAGOOL_AUDIO_TRACK_COUNT {len(tracks_info)}
#define GABAGOOL_MUSIC_TRACK_COUNT {len(music_track_indexes)}
#define GABAGOOL_GUBBY_SFX_TRACK_INDEX {gubby_sfx_track_index}

static const uint32_t GABAGOOL_IMAGE_RESOURCE_IDS[GABAGOOL_IMAGE_COUNT] = {{
  {image_ids}
}};

static const uint16_t GABAGOOL_IMAGE_WIDTHS[GABAGOOL_IMAGE_COUNT] = {{
  {widths_str}
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

static const uint8_t GABAGOOL_MUSIC_TRACK_INDEXES[GABAGOOL_MUSIC_TRACK_COUNT] = {{
  {music_track_indexes_str}
}};
""")


def main():
    parser = argparse.ArgumentParser(description="Generate dithered Pebble assets and packed watch audio.")
    parser.parse_args()

    GENERATED.mkdir(parents=True, exist_ok=True)
    icon_entry, image_entries, image_widths = convert_images()
    audio_entries, tracks_info = convert_audio()
    pet_entries = convert_pet_assets()
    update_package(icon_entry, image_entries, audio_entries, pet_entries)
    write_header(image_entries, image_widths, tracks_info)
    
    total_samples = sum(t["total_samples"] for t in tracks_info)
    encoded_bytes = sum(t["encoded_bytes"] for t in tracks_info)
    print(f"Generated {len(image_entries)} dithered images and {len(audio_entries)} audio chunks.")
    print(f"Audio: {len(tracks_info)} tracks, {total_samples / AUDIO_SOURCE_RATE:.1f}s, {encoded_bytes} encoded bytes.")


if __name__ == "__main__":
    main()
