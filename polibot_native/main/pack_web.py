"""Packs one of the panel's text files with gzip, for the build: pack_web.py <source> <packed>."""
import gzip
import sys
from pathlib import Path

# mtime=0: the same source gives the same bytes in every build
Path(sys.argv[2]).write_bytes(gzip.compress(Path(sys.argv[1]).read_bytes(), 9, mtime=0))
