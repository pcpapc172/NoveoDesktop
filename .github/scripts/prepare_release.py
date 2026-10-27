import sys
from pathlib import Path


def without_symbol_tool(source):
    commands = r'''    cd tools\\windows\\dump_syms
    gyp dump_syms.gyp --format=msvs
    msbuild -m dump_syms.vcxproj /property:Configuration=Release /property:Platform="x64" %ToolsetProp%
'''
    if source.count(commands) != 1:
        raise RuntimeError('The Breakpad symbol tool commands changed; review release preparation.')
    return source.replace(commands, '', 1)


def release_qt_only(source):
    start = source.index("else: # qt > '6'")
    end = source.index("stage('tg_owt'", start)
    qt = source[start:end]
    changes = (
        ('    SET CONFIGURATIONS=-debug-and-release\n', '    SET CONFIGURATIONS=-release\n'),
        ('        -force-debug-info ^\n', ''),
        ('    cmake --build . --config Debug\n    cmake --install . --config Debug\n'
         '    cmake --build .\n    cmake --install .\n',
         '    cmake --build . --config Release\n    cmake --install . --config Release\n'),
    )
    for before, after in changes:
        if qt.count(before) != 1:
            raise RuntimeError('The Qt preparation commands changed; review release preparation.')
        qt = qt.replace(before, after, 1)
    return source[:start] + qt + source[end:]


if __name__ == '__main__':
    script = Path(__file__).resolve().parents[2] / 'Telegram/build/prepare/prepare.py'
    code = release_qt_only(without_symbol_tool(script.read_text(encoding='utf-8')))
    sys.argv = [str(script), *sys.argv[1:]]
    exec(compile(code, str(script), 'exec'), {'__file__': str(script), '__name__': '__main__'})
