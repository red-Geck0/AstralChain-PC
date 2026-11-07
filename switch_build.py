"""Test build only: switches build\\game between the builds with and without function-lifter, without rebuilding.
The launcher always plays build\\game, so all its settings keep working. userdata\\functions-off.txt marks the build
without function-lifter (setup_game.py reads it)."""
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent
BUILD = PROJECT/'build'
ACTIVE = BUILD/'game'
WITH = BUILD/'game-function-lifter'
WITHOUT = BUILD/'game-tanpa-function-lifter'
MARKER = PROJECT/'userdata'/'functions-off.txt'


def stamp():
    """The active build matches the sources and the marker: the launcher must not rebuild it."""
    from astral_chain import setup_game
    (ACTIVE/'build-fingerprint.txt').write_text(setup_game.fingerprint() + '\n', encoding='ascii')


def main():
    if '--stamp' in sys.argv:
        stamp()
        return 0
    off = MARKER.is_file()
    keep, use = (WITHOUT, WITH) if off else (WITH, WITHOUT)
    if not (use/'round-00'/'trace.exe').is_file():
        print(f'Build tidak ditemukan: {use}')
        return 1
    try:
        ACTIVE.rename(keep)
        use.rename(ACTIVE)
    except PermissionError:
        if keep.is_dir() and not ACTIVE.exists():
            keep.rename(ACTIVE)
        print('Tidak bisa ganti build. Tutup game dan launcher dulu, lalu coba lagi.')
        return 1
    if off:
        MARKER.unlink()
    else:
        MARKER.write_text('Build tanpa function-lifter aktif (switch_build.py).\n', encoding='ascii')
    stamp()
    print('Sekarang aktif: ' + ('DENGAN function-lifter' if off else 'TANPA function-lifter'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
