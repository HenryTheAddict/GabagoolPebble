#!/usr/bin/env python3
"""Compatibility entrypoint for the gubby audio metadata normalizer."""

import runpy
import sys
from pathlib import Path


sys.dont_write_bytecode = True
script = Path(__file__).with_name("fix_gubby_audio_metadata.py")
namespace = runpy.run_path(str(script))
raise SystemExit(namespace["main"]())
