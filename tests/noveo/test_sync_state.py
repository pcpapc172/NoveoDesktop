from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
flags=subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Core'],text=True).split()
with tempfile.TemporaryDirectory(prefix='noveo-sync-state-') as d:
 binary=Path(d)/'probe'
 subprocess.run(['g++','-std=c++20','-fPIC','-Wall','-Wextra','-Werror','-I'+str(root/'Telegram/SourceFiles'),str(root/'tests/noveo/sync_state_probe.cpp'),str(root/'Telegram/SourceFiles/noveo/sync_state.cpp'),'-o',str(binary),*flags],check=True)
 subprocess.run([str(binary)],check=True)
