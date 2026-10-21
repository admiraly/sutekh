"""Compare every PCM buffer and saved checkpoint against uncached spatial controls."""

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(dir=Path.cwd()) as directory:
    outputs = []
    for index, executable in enumerate(sys.argv[1:]):
        output = Path(directory) / f"controls-{index}.bin"
        subprocess.run([executable, str(output)], check=True)
        outputs.append(output.read_bytes())
    if len(outputs) != 2 or outputs[0] != outputs[1]:
        raise SystemExit("cached/uncached PCM or checkpoint bytes differ")
    print(f"bit-identical PCM/checkpoints: {len(outputs[0])} bytes, "
          f"sha256 {hashlib.sha256(outputs[0]).hexdigest()}")
