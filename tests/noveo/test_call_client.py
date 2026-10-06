"""Compile and run the actual call controller without a live account or microphone."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='noveo-call-') as directory:
    binary = Path(directory) / 'probe'
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt6Core'], text=True).split()
    subprocess.run(['g++', '-std=c++20', '-fPIC', '-DQT_NO_KEYWORDS',
        '-I' + str(ROOT / 'Telegram/SourceFiles'),
        str(ROOT / 'tests/noveo/call_client_probe.cpp'),
        str(ROOT / 'Telegram/SourceFiles/noveo/call_client.cpp'), '-o', str(binary), *flags], check=True)
    subprocess.run([str(binary)], check=True)
