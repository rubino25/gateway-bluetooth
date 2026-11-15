"""Rende importabili dai test i moduli di questa cartella (protocol, mapping, ...)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
