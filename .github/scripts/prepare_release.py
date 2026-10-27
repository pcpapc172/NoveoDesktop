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


if __name__ == '__main__':
    script = Path(__file__).resolve().parents[2] / 'Telegram/build/prepare/prepare.py'
    code = without_symbol_tool(script.read_text(encoding='utf-8'))
    sys.argv = [str(script), *sys.argv[1:]]
    exec(compile(code, str(script), 'exec'), {'__file__': str(script), '__name__': '__main__'})
