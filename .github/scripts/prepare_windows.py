"""Apply the pinned Windows 7 Qt backport without changing other stage keys."""
from pathlib import Path
import sys

PATCH_REVISION = '9ce1ffe051161a22f7e78f715b042c773f868e59'


def with_windows7(source):
    start = source.index("else: # qt > '6'")
    end = source.index("stage('tg_owt'", start)
    qt = source[start:end]
    anchor = "    stage('qt_' + qt, \"\"\"\n"
    if qt.count(anchor) != 1:
        raise RuntimeError('The Qt 6 preparation stage changed.')
    stage = '''    if win and qt != '6.11.2':
        error('Review Windows 7 patches before changing Qt version.')
    stage('qt6_windows7', """
win:
    git clone https://github.com/desktop-app/qt6_windows7_patches.git qt6_windows7
    cd qt6_windows7
    git checkout ''' + PATCH_REVISION + '''
""")

'''
    qt = qt.replace(anchor, stage + anchor, 1)
    anchor = 'win:\n    cd qtbase\n    setlocal enabledelayedexpansion\n'
    if qt.count(anchor) != 1:
        raise RuntimeError('The Qt patch ordering changed.')
    patched = r'''win:
depends:qt6_windows7/*.patch
    cd qtbase
    setlocal enabledelayedexpansion
    for %%i in (..\\..\\qt6_windows7\\*.patch) do (
        git apply %%i --ignore-whitespace -v
        if errorlevel 1 (
            echo ERROR: Applying Windows 7 patch %%~nxi failed!
            exit /b 1
        )
    )
'''
    qt = qt.replace(anchor, patched, 1)
    return source[:start] + qt + source[end:]


if __name__ == '__main__':
    script = Path(__file__).resolve().parents[2] / 'Telegram/build/prepare/prepare.py'
    code = with_windows7(script.read_text(encoding='utf-8'))
    sys.argv = [str(script), *sys.argv[1:]]
    exec(compile(code, str(script), 'exec'), {'__file__': str(script), '__name__': '__main__'})
