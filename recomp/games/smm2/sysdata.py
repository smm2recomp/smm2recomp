"""Copy the system data the game reads from the player's own firmware (optional): shared fonts and data archives.

    fonts.py --firmware <registered NCA folder> --keys <prod.keys> --data <data root> [--hactool PATH]

Writes <data root>/fonts/*.bfttf (the console's shared fonts, nn::pl: runtime/hle/pl.c substitutes a host font
without them, which looks different) and <data root>/sysdata/<title id>/ (the RomFS of system data archives the game
opens by data id, e.g. the Mii model data; runtime/services/fs.c serves them). The firmware is the NCA folder of a
system dump, e.g. Ryujinx's bis/system/Contents/registered. Keys are only handed to hactool and read in place;
hactool's output is never shown (it can echo key values), only whether each step worked. Lines for the launcher
(runtime/ui/launcher.mm) start with '@', as setup.py's: "@progress <0..1>", "@error <code>\t<what>\t<what to do>".
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from setup import HACTOOL_CRASHES, masked  # noqa: E402  (setup.py: the crash codes, output without key values)

FONT_TITLES = {
    '0100000000000810',  # NintendoExtension (button glyphs)
    '0100000000000811',  # Standard (Latin + Japanese)
    '0100000000000812',  # Korean
    '0100000000000813',  # Chinese (traditional)
    '0100000000000814',  # Chinese (simplified, + extension)
}
DATA_TITLES = {
    '0100000000000802',  # MiiModel: Mii face parts (nn::mii renders the players' Miis)
    '0100000000000806',  # NgWord (nn::ngc: text filter)
    '010000000000080e',  # TimeZoneBinary
    '0100000000000823',  # NgWord2
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace')


def fail(code, what, hint):
    print(f'@error {code}\t{what}\t{hint}', flush=True)
    sys.exit(f'sysdata: {what}')


def nca_path(entry):
    # Ryujinx keeps each NCA as a folder <id>.nca/00; a plain dump has files
    return os.path.join(entry, '00') if os.path.isdir(entry) else entry


FAILS = []  # (exit code, output) of the NCAs hactool couldn't read, for the error
EMPTY = {}  # data titles found but extracted to nothing: title -> hactool's last words
MII_MODEL = '0100000000000802'  # (required: SMM2 stops at its first Mii without it, issue #28)
REVISION = {}  # NCA path -> the master key its contents are encrypted with (hactool's "Master Key Revision")
# the firmware that first used each master key (21.0.0 = 0x14 checked on a dump; later ones are left unnamed)
FIRST_FIRMWARE = {0x0: '1.0.0', 0x1: '3.0.0', 0x2: '3.0.1', 0x3: '4.0.0', 0x4: '5.0.0', 0x5: '6.0.0', 0x6: '6.2.0',
                  0x7: '7.0.0', 0x8: '8.1.0', 0x9: '9.0.0', 0xa: '9.1.0', 0xb: '12.1.0', 0xc: '13.0.0', 0xd: '14.0.0',
                  0xe: '15.0.0', 0xf: '16.0.0', 0x10: '17.0.0', 0x11: '18.0.0', 0x12: '19.0.0', 0x13: '20.0.0',
                  0x14: '21.0.0'}


def title_id(hactool, keys, nca):
    r = run([hactool, '-k', keys, '--disablekeywarns', '-t', 'nca', nca])
    tid = None
    for line in r.stdout.splitlines():
        s = line.strip()
        if s.startswith('Title ID:'):
            tid = s.split(':', 1)[1].strip().lower()
        elif s.startswith('Master Key Revision:'):  # "0x14 (Unknown)": a number, not a key value
            try:
                REVISION[nca] = int(s.split(':', 1)[1].split()[0], 16)
            except (IndexError, ValueError):
                pass
    if tid:
        return tid
    FAILS.append((r.returncode, r.stdout + r.stderr))
    return None


def master_keys_in(keys):
    """The master key numbers prod.keys has, directly or as the kek hactool makes one from (names only)."""
    have = set()
    with open(keys, errors='replace') as f:
        for line in f:
            m = re.match(r'\s*master_ke[yk]_([0-9a-fA-F]{2})\s*=', line)
            if m:
                have.add(int(m.group(1), 16))
    return have


def keys_older_than(keys, ncas):
    """None, or the error for a prod.keys without the master key these NCAs' contents need: its header key opens
    a newer firmware's headers (title ids are read) but the contents come out as garbage ("section 0 is corrupted")."""
    need = max((REVISION.get(p, 0) for p in ncas), default=0)
    have = master_keys_in(keys)
    if not need or need in have:
        return None
    def name(n):
        return f'master_key_{n:02x}' + (f' (firmware {FIRST_FIRMWARE[n]})' if n in FIRST_FIRMWARE else '')
    top = max(have) if have else None
    return (f'prod.keys is older than the firmware: its system files need {name(need)}, and the newest master key in '
            f'prod.keys is ' + (name(top) if top is not None else 'missing'))


def why_none():
    """What hactool said about the NCAs it couldn't read: a crash (no output, an exit code), or its last words."""
    if not FAILS:
        return ''
    if any('Invalid NCA header' in out for rc, out in FAILS):  # (masked() drops it: "Are keys correct?")
        return ' (prod.keys doesn\'t open them: the header key is missing or wrong)'
    silent = [rc for rc, out in FAILS if not out.strip()]
    if len(silent) == len(FAILS):
        code = silent[0] & 0xFFFFFFFF
        return f' (hactool stopped without a message: {HACTOOL_CRASHES.get(code, f"exit code {silent[0]} ({code:#x})")})'
    tail = [ln for rc, out in FAILS for ln in masked(out).splitlines() if ln.strip()][-2:]
    return f' (hactool: {" / ".join(tail)})' if tail else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--firmware', required=True)
    ap.add_argument('--keys', required=True)
    ap.add_argument('--data', required=True)
    ap.add_argument('--hactool', default=shutil.which('hactool') or os.path.expanduser('~/smm-work/tools/bin/hactool'))
    a = ap.parse_args()
    if not a.hactool or not os.path.exists(a.hactool):
        fail('NO_HACTOOL', 'hactool was not found', 'It comes with this program: reinstall it, or pass --hactool.')
    if not os.path.isfile(a.keys):
        fail('NO_KEYS', 'prod.keys was not found', 'Choose your console\'s prod.keys (dumped with Lockpick_RCM).')
    if not os.path.isdir(a.firmware):
        fail('NO_FIRMWARE', 'The firmware folder was not found',
             'Choose the folder of your console\'s system NCAs, e.g. Ryujinx\'s bis/system/Contents/registered.')
    want = FONT_TITLES | DATA_TITLES
    found = {}
    ncas = sorted(n for n in os.listdir(a.firmware) if n.endswith('.nca'))
    if not ncas:
        fail('NO_FIRMWARE', 'The firmware folder has no NCA files',
             'Choose the folder of your console\'s system NCAs, e.g. Ryujinx\'s bis/system/Contents/registered '
             '(after installing your firmware dump in Ryujinx), or the registered folder of a NAND dump.')
    for i, name in enumerate(ncas):
        print(f'@progress {0.8 * i / len(ncas):.3f}', flush=True)
        p = nca_path(os.path.join(a.firmware, name))
        if not os.path.isfile(p) or os.path.getsize(p) < 0x8000:  # (data archives are larger: skip tiny NCAs quickly)
            continue
        tid = title_id(a.hactool, a.keys, p)
        if tid in want:
            found[tid] = p
        if len(found) == len(want):
            break
    fonts = os.path.join(a.data, 'fonts')
    os.makedirs(fonts, exist_ok=True)
    nf = nd = 0
    for tid, p in sorted(found.items()):
        if tid in FONT_TITLES:
            with tempfile.TemporaryDirectory() as tmp:
                run([a.hactool, '-k', a.keys, '--disablekeywarns', '-t', 'nca', '--romfsdir', tmp, p])
                for f in os.listdir(tmp):
                    if f.endswith('.bfttf'):
                        shutil.copy(os.path.join(tmp, f), os.path.join(fonts, f))
                        nf += 1
        else:
            out = os.path.join(a.data, 'sysdata', tid)
            if os.path.isdir(out):
                shutil.rmtree(out)
            os.makedirs(out)
            r = run([a.hactool, '-k', a.keys, '--disablekeywarns', '-t', 'nca', '--romfsdir', out, p])
            if os.listdir(out):
                nd += 1
            else:  # (read but nothing came out: the keys open the header, not the contents - an older prod.keys)
                shutil.rmtree(out)
                tail = [ln for ln in masked(r.stdout + r.stderr).splitlines() if ln.strip()][-2:]
                EMPTY[tid] = ' / '.join(tail)
    missing = sorted(want - set(found))
    print(f'sysdata: {nf} shared font files, {nd} data archives'
          + (f' (not in this firmware: {", ".join(missing)})' if missing else ''))
    print('@progress 1', flush=True)
    old = keys_older_than(a.keys, [p for tid, p in found.items() if tid in EMPTY or tid in FONT_TITLES and not nf])
    if old:
        fail('KEYS_OLD', old,
             'Dump prod.keys again from your console with a Lockpick_RCM version that supports its firmware version '
             '(an older Lockpick_RCM still writes a prod.keys, without the newest keys), then run setup again.')
    if not found:
        why = why_none()
        print(f'sysdata: {len(FAILS)} NCAs unreadable{why}', flush=True)
        if 'without a message' in why:
            fail('HACTOOL_CRASH', 'The firmware couldn\'t be read' + why,
                 'Please report this with the setup log. hactool, which comes with this program, didn\'t run '
                 'properly on this computer.')
        fail('KEYS', 'No system titles could be read from the firmware' + why,
             'prod.keys must be your console\'s and hold the keys for that firmware version (dump them again with a '
             'current Lockpick_RCM after updating). A newer firmware than this program\'s hactool knows can also do this.')
    if MII_MODEL not in found or MII_MODEL in EMPTY:
        why = f' (hactool: {EMPTY[MII_MODEL]})' if EMPTY.get(MII_MODEL) else ''
        fail('MII_DATA', 'The console\'s Mii data (0100000000000802) couldn\'t be read from the firmware' + why,
             'The game needs it to draw Miis. Choose the complete folder of your console\'s system NCAs (e.g. '
             'Ryujinx\'s bis/system/Contents/registered) and the prod.keys dumped from the same console after its last '
             'update (a current Lockpick_RCM): an older prod.keys opens a newer firmware\'s headers but not its contents.')
    if 'nintendo_udsg-r_std_003.bfttf' not in os.listdir(fonts):
        fail('FONTS_MISSING', 'The console\'s shared fonts were not found in the firmware',
             'Choose the folder of your console\'s system NCAs (with the shared font titles 0100000000000810-814).')


if __name__ == '__main__':
    main()
