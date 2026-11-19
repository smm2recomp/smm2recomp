"""Lay out the player's own Super Mario Maker 2 v3.0.3 files for the recomp.

From extracted folders (`make setup`):

    setup.py --base-romfs <dir> --update-exefs <dir> --update-romfs <dir> --out <dir>

From whatever the player has (the launcher, runtime/ui/launcher.mm): folders with the extracted ExeFS / RomFS, or the
NSP / XCI files themselves, which are extracted with hactool and the player's own prod.keys:

    setup.py --source <folder | .nsp | .xci> [--source ...] [--keys prod.keys] [--hactool PATH] --out <dir>

<out>/exefs: the v3.0.3 main, sdk, rtld and main.npdm (copied; checked against the known v3.0.3 build ids and hashes,
             versions.py)
<out>/romfs: the base game's RomFS with the update's files over it, as a tree of links (nothing copied;
             runtime/services/fs.c serves it without building an image); extracted from an NSP / XCI: the patched
             RomFS itself
<out>/setup.json: what was used (paths, versions, build ids; never keys)

Keys are only handed to hactool and read in place; hactool's output is never shown (it can echo key values). Lines
for the launcher start with '@': "@step <text>", "@progress <0..1>", "@error <code>\t<what>\t<what to do>".
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import versions  # noqa: E402

EXEFS_FILES = versions.EXEFS_FILES
BUILD_IDS = versions.BUILD_IDS['3.0.3']  # (make's setup and older callers)


class SetupError(Exception):
    def __init__(self, code, what, hint=''):
        super().__init__(what)
        self.code, self.what, self.hint = code, what, hint


def say(kind, text=''):
    print(f'@{kind} {text}'.rstrip(), flush=True)


def masked(text):
    """hactool / tool output made safe to show: no key values, no lines about keys."""
    out = []
    for line in text.splitlines():
        if re.search(r'key', line, re.IGNORECASE):
            continue
        out.append(re.sub(r'[0-9a-fA-F]{16,}', '<hex>', line))
    return '\n'.join(out)


# ------------------------------------------------------------------ ExeFS
def build_id(path):
    with open(path, 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        return None
    return head[0x40:0x60].rstrip(b'\0').hex()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def exefs_version(d):
    """The game version an ExeFS folder is (by main's build id), or None."""
    p = os.path.join(d, 'main')
    try:
        return versions.version_of_build_id('main', build_id(p))
    except OSError:
        return None


def check_exefs(src):
    """Raises SetupError unless src holds the v3.0.3 ExeFS."""
    for name in EXEFS_FILES:
        p = os.path.join(src, name)
        if not os.path.isfile(p):
            raise SetupError('MISSING', f'{name} is missing from the ExeFS folder {src}',
                             'Extract the 3.0.3 update\'s ExeFS (main, sdk, rtld, main.npdm) there.')
    for name in ('main', 'sdk'):
        p = os.path.join(src, name)
        got = build_id(p)
        if got is None:
            raise SetupError('NOT_NSO', f'{p} is not a Switch executable', 'Choose the folder of the extracted ExeFS.')
        if not got.startswith(versions.BUILD_IDS['3.0.3'][name]):
            ver = versions.version_of_build_id(name, got)
            if ver:
                raise SetupError('WRONG_VERSION', f'These game files are version {ver}',
                                 'The recomp needs version 3.0.3: add the 3.0.3 update (its NSP, or its extracted '
                                 'ExeFS and RomFS).')
            raise SetupError('WRONG_VERSION', f'{name} is not Super Mario Maker 2 3.0.3 (build id {got[:16]}...)',
                             'Choose the files of Super Mario Maker 2 with the 3.0.3 update.')
    for name, want in versions.EXEFS_SHA256.items():
        if sha256(os.path.join(src, name)) != want:
            raise SetupError('BAD_HASH', f'{name} doesn\'t match the known 3.0.3 file',
                             'The file may be damaged or modified (mods, cheats). Extract the update again.')


def copy_exefs(src, dst, check_hashes=True):
    if check_hashes:
        check_exefs(src)
    os.makedirs(dst, exist_ok=True)
    for name in EXEFS_FILES:
        p = os.path.join(src, name)
        if not os.path.isfile(p):
            sys.exit(f'setup: {p} is missing (extract the v3.0.3 update\'s exefs there)')
        want = BUILD_IDS.get(name)
        if want:
            got = build_id(p)
            if got is None:
                sys.exit(f'setup: {p} is not an NSO')
            if not got.startswith(want):
                sys.exit(f'setup: {p} is not the v3.0.3 {name} (build id {got[:16]}..., want {want[:16]}...)')
        q = os.path.join(dst, name)
        if not (os.path.exists(q) and os.path.getsize(q) == os.path.getsize(p)
                and os.path.getmtime(q) >= os.path.getmtime(p)):
            shutil.copy2(p, q)
    print(f'setup: exefs ok ({", ".join(EXEFS_FILES)})')


# ------------------------------------------------------------------ RomFS
def place(target, link):
    """A symlink; where the OS won't make one (Windows without Developer Mode), a hard link (same drive), else a copy."""
    try:
        os.symlink(target, link)
        return
    except OSError:
        pass
    try:
        os.link(target, link)
    except OSError:
        shutil.copy2(target, link)


def link_tree(src, dst, counts):
    """Symlink every file of src into dst (dst's existing links are replaced: later trees win)."""
    for root, dirs, files in os.walk(src, followlinks=True):
        rel = os.path.relpath(root, src)
        out = os.path.join(dst, rel) if rel != '.' else dst
        os.makedirs(out, exist_ok=True)
        dirs[:] = [d for d in dirs if not d.startswith('.')]
        for f in files:
            if f.startswith('.'):
                continue
            target = os.path.realpath(os.path.join(root, f))
            link = os.path.join(out, f)
            if os.path.islink(link):
                if os.readlink(link) == target:
                    counts['same'] += 1
                    continue
                os.remove(link)
                counts['replaced'] += 1
            elif os.path.exists(link):  # (a hard link or copy from place(): replaced the same way)
                if os.path.samefile(link, target):
                    counts['same'] += 1
                    continue
                os.remove(link)
                counts['replaced'] += 1
            place(target, link)
            counts['linked'] += 1


ROMFS_MARKERS = ('Pack', 'System', 'Layout')  # (top-level folders of SMM2's RomFS)


def is_romfs(d):
    return all(os.path.isdir(os.path.join(d, m)) for m in ROMFS_MARKERS)


def is_exefs(d):
    return os.path.isfile(os.path.join(d, 'main')) and os.path.isfile(os.path.join(d, 'main.npdm'))


# ------------------------------------------------------------------ NSP / XCI (hactool, the player's keys)
def pfs0_entries(f, base=0):
    """[(name, offset, size)] of a PFS0 (NSP) or HFS0 (XCI partition) at base."""
    f.seek(base)
    head = f.read(0x10)
    magic = head[:4]
    if magic not in (b'PFS0', b'HFS0'):
        return None
    n, strsize = struct.unpack_from('<II', head, 4)
    esize = 0x18 if magic == b'PFS0' else 0x40
    table = f.read(n * esize)
    strings = f.read(strsize)
    data = base + 0x10 + n * esize + strsize
    out = []
    for i in range(n):
        off, size, name_off = struct.unpack_from('<QQI', table, i * esize)
        name = strings[name_off:strings.index(b'\0', name_off)].decode('utf-8', 'replace')
        out.append((name, data + off, size))
    return out


def container_entries(path):
    """The files of an NSP, or of an XCI's secure partition: [(name, offset, size)]."""
    with open(path, 'rb') as f:
        e = pfs0_entries(f)
        if e is not None:
            return e
        f.seek(0x100)
        if f.read(4) != b'HEAD':
            raise SetupError('NOT_CONTAINER', f'{os.path.basename(path)} is not an NSP or XCI',
                             'Choose the game\'s NSP or XCI file, or the folder of its extracted files.')
        f.seek(0x130)
        root_off, = struct.unpack('<Q', f.read(8))
        root = pfs0_entries(f, root_off) or []
        for name, off, _ in root:
            if name == 'secure':
                return pfs0_entries(f, off) or []
    raise SetupError('NOT_CONTAINER', f'{os.path.basename(path)}: no game partition found', 'Dump the cartridge again.')


def copy_range(src, off, size, dst, progress=None):
    with open(src, 'rb') as f, open(dst, 'wb') as o:
        f.seek(off)
        left = size
        while left:
            b = f.read(min(left, 8 << 20))
            if not b:
                raise SetupError('TRUNCATED', f'{os.path.basename(src)} is shorter than its contents say',
                                 'The file is incomplete: dump it again.')
            o.write(b)
            left -= len(b)
            if progress:
                progress(size - left)


# Windows' status codes for a program that never got going or crashed (hactool printing nothing)
HACTOOL_CRASHES = {0xC0000005: 'it crashed: access violation', 0xC00000FD: 'it crashed: stack overflow',
                   0xC0000135: 'a DLL it needs is missing', 0xC0000139: 'a DLL it needs is the wrong version',
                   0xC0000142: 'it could not start', 0xC000007B: 'it is built for another kind of Windows',
                   0xC0000409: 'it crashed: buffer overrun', 0xC0000374: 'it crashed: heap corruption'}
# ... and the signals that stop it on Linux and macOS (a negative return code)
HACTOOL_SIGNALS = {11: 'it crashed: segmentation fault', 6: 'it crashed: abort',
                   7: 'it crashed: bus error (the file or its drive went away while it was read?)',
                   4: 'it crashed: illegal instruction (a CPU it wasn\'t built for?)', 9: 'it was killed (out of memory?)',
                   8: 'it crashed: arithmetic error'}


BAD_KEYS_HINT = ('Choose the prod.keys dumped from your own console with a current Lockpick_RCM: it must have the keys '
                 'for this game\'s version. The prod.keys of an emulator that runs the game will also do.')


class Hactool:
    def __init__(self, exe, keys):
        if not exe or not os.path.isfile(exe):
            raise SetupError('NO_HACTOOL', 'hactool was not found',
                             'Extracting an NSP or XCI needs hactool, which comes with this program. Reinstall it, or '
                             'extract the files yourself and choose the folders.')
        if not keys or not os.path.isfile(keys):
            raise SetupError('NO_KEYS', 'prod.keys is needed to extract an NSP or XCI',
                             'Choose your console\'s prod.keys (dumped with Lockpick_RCM).')
        self.exe, self.keys = exe, keys

    def run(self, *args, what='hactool'):
        r = subprocess.run([self.exe, '-k', self.keys, '--disablekeywarns', *args], capture_output=True,
                           encoding='utf-8', errors='replace', text=True)
        out = r.stdout + r.stderr
        if r.returncode != 0 or 'Invalid NCA header' in out or 'Failed to open' in out:
            # (hactool's own wording, masked: never key values)
            tail = [ln for ln in masked(out).splitlines() if ln.strip()][-3:]
            say('log', f'hactool {" ".join(masked(a) for a in args)}: exit code {r.returncode}')
            if 'Invalid NCA header' in out:
                # ("... Are keys correct?": masked() drops it as a line about keys, which made it look like a crash)
                raise SetupError('KEYS', f'{what} failed: prod.keys doesn\'t open its files (the header key is '
                                 'missing or wrong)', BAD_KEYS_HINT)
            if not tail:
                # hactool said nothing at all: it didn't start or it crashed, which isn't the keys' fault
                code = r.returncode & 0xFFFFFFFF
                why = (HACTOOL_SIGNALS.get(-r.returncode, f'it was stopped by signal {-r.returncode}')
                       if r.returncode < 0 else HACTOOL_CRASHES.get(code, f'exit code {r.returncode} ({code:#x})'))
                raise SetupError('HACTOOL_CRASH', f'{what} failed: hactool stopped without a message ({why})',
                                 'Please report this with the setup log. Meanwhile you can extract the game '
                                 'yourself (an emulator\'s "Extract ExeFS / RomFS" of the updated game) and choose '
                                 'the folders instead of the NSP / XCI.')
            raise SetupError('HACTOOL', f'{what} failed: {" / ".join(tail)}', BAD_KEYS_HINT +
                             ' Check also that the file is complete.')
        return out


def parse_cnmt(data):
    tid, ver, typ = struct.unpack_from('<QIB', data, 0)
    ext_size, count = struct.unpack_from('<HH', data, 0xE)
    contents = []
    for i in range(count):
        off = 0x20 + ext_size + i * 0x38
        ncaid = data[off + 0x20:off + 0x30].hex()
        size = int.from_bytes(data[off + 0x30:off + 0x36], 'little')
        ctype = data[off + 0x36]
        contents.append((ncaid, size, ctype))
    return {'title_id': tid, 'version': ver, 'type': typ, 'contents': contents}


def read_title(path, entries, hac, stage):
    """The metadata of an NSP / XCI: its title(s), each with the program NCA's entry and the title key (NSP)."""
    titles = []
    byname = {n: (o, s) for n, o, s in entries}
    for name, off, size in entries:
        if not name.endswith('.cnmt.nca'):
            continue
        tmp = os.path.join(stage, 'meta')
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        nca = os.path.join(tmp, name)
        copy_range(path, off, size, nca)
        hac.run('-t', 'nca', f'--section0dir={tmp}/cnmt', nca, what=f'Reading {os.path.basename(path)}')
        cnmts = [f for f in os.listdir(os.path.join(tmp, 'cnmt')) if f.endswith('.cnmt')] if os.path.isdir(
            os.path.join(tmp, 'cnmt')) else []
        if not cnmts:
            continue
        with open(os.path.join(tmp, 'cnmt', cnmts[0]), 'rb') as f:
            meta = parse_cnmt(f.read())
        shutil.rmtree(tmp, ignore_errors=True)
        prog = [c for c in meta['contents'] if c[2] == 1]  # (content type 1: Program)
        if not prog:
            continue
        pname = prog[0][0] + '.nca'
        if pname not in byname:
            raise SetupError('INCOMPLETE', f'{os.path.basename(path)} lacks its program ({pname})',
                             'The file is incomplete: dump it again.')
        t = {'title_id': meta['title_id'], 'version': meta['version'], 'type': meta['type'],
             'program': (pname, *byname[pname]), 'titlekey': None}
        # the title key of an eShop title (a cartridge's has none): the ticket whose rights id names this title
        for tname, toff, tsize in entries:
            if not tname.endswith('.tik') or tsize < 0x2C0:
                continue
            with open(path, 'rb') as f:
                f.seek(toff)
                tik = f.read(0x2C0)
            if tik[0x2A0:0x2A8] != struct.pack('>Q', meta['title_id']):
                continue
            if tik[0x281] != 0:
                raise SetupError('PERSONAL_TICKET', f'{os.path.basename(path)} has a personalized ticket',
                                 'Dump the game again with a tool that writes a standard ticket (e.g. nxdumptool), or '
                                 'extract it yourself and choose the folders.')
            t['titlekey'] = tik[0x180:0x190].hex()
        titles.append(t)
    return titles


def watch_size(path, total, lo, hi, stop):
    """@progress lo..hi while a folder or file grows towards total bytes (hactool says nothing while it works)."""
    while not stop.wait(1.0):
        n = 0
        if os.path.isfile(path):
            n = os.path.getsize(path)
        elif os.path.isdir(path):
            for root, _, files in os.walk(path):
                for f in files:
                    try:
                        n += os.path.getsize(os.path.join(root, f))
                    except OSError:
                        pass
        say('progress', f'{lo + (hi - lo) * min(1.0, n / max(total, 1)):.3f}')


def hactool_watched(hac, out_path, total, lo, hi, *args, what):
    stop = threading.Event()
    t = threading.Thread(target=watch_size, args=(out_path, total, lo, hi, stop), daemon=True)
    t.start()
    try:
        hac.run(*args, what=what)
    finally:
        stop.set()
        t.join()


def need_space(where, need):
    free = shutil.disk_usage(where).free
    if free < need:
        raise SetupError('DISK', f'Not enough free disk space: {need / 1e9:.1f} GB needed, {free / 1e9:.1f} GB free',
                         'Free some space on the drive with the data folder and try again.')


def extract_containers(files, hac, stage):
    """NSP / XCI files -> {'exefs': dir, 'romfs': dir, 'found': [...]} in stage (RomFS patched by the update)."""
    base = upd = None
    found = []
    for path in files:
        entries = container_entries(path)
        for t in read_title(path, entries, hac, stage):
            tid, ver = t['title_id'], t['version']
            label = f'{os.path.basename(path)}: {versions.TITLE if tid in (versions.BASE_TITLE_ID, versions.UPDATE_TITLE_ID) else f"title {tid:016x}"}'
            if tid == versions.BASE_TITLE_ID:
                found.append(f'{label} {versions.display_version(tid, ver)}')
                base = (path, t)
            elif tid == versions.UPDATE_TITLE_ID:
                found.append(f'{label} update {versions.display_version(tid, ver)}')
                if upd is None or ver > upd[1]['version']:
                    upd = (path, t)
            else:
                found.append(f'{label} (not used)')
    for f in found:
        say('found', f)
    if base is None and upd is None:
        raise SetupError('NOT_SMM2', 'No Super Mario Maker 2 in the chosen files',
                         'Choose your Super Mario Maker 2 NSP / XCI and its 3.0.3 update.')
    if base is None:
        raise SetupError('NO_BASE', 'Only the update was chosen',
                         'The update holds only what changed: also choose the game itself (its NSP or XCI, or its '
                         'extracted RomFS).')
    if upd is not None and upd[1]['version'] != versions.UPDATE_VERSION:
        v = versions.display_version(versions.UPDATE_TITLE_ID, upd[1]['version'])
        raise SetupError('WRONG_VERSION', f'The update is {v}, not 3.0.3',
                         'The recomp needs the 3.0.3 update: choose its NSP.')
    if upd is None:
        raise SetupError('WRONG_VERSION', f'These game files are version '
                         f'{versions.display_version(versions.BASE_TITLE_ID, base[1]["version"])}',
                         'The recomp needs version 3.0.3: also choose the 3.0.3 update\'s NSP.')
    os.makedirs(stage, exist_ok=True)
    bpath, bt = base
    bname, boff, bsize = bt['program']
    need_space(stage, bsize * 2 + (upd[1]['program'][2] if upd else 0) + (1 << 30))
    nca = os.path.join(stage, 'base.nca')
    say('step', 'Reading the game from ' + os.path.basename(bpath))
    copy_range(bpath, boff, bsize, nca, lambda n: None)
    tk = [f'--titlekey={bt["titlekey"]}'] if bt['titlekey'] else []
    exefs, romfs = os.path.join(stage, 'exefs'), os.path.join(stage, 'romfs')
    for d in (exefs, romfs):
        shutil.rmtree(d, ignore_errors=True)
    # The base is its whole decrypted RomFS section (-r --section1), hash levels and all, not the bare RomFS image
    # (--romfs): the update's relocation table addresses the base in section offsets (3.0.3's: the RomFS data
    # starts 0x588000 into the section), and hactool's --baseromfs reads them as they are.
    raw = os.path.join(stage, 'base.romfs')
    say('step', 'Extracting the game files')
    hactool_watched(hac, raw, bsize, 0.05, 0.3, *tk, '-t', 'nca', '-r', f'--section1={raw}', nca, what='Extracting the game')
    os.remove(nca)
    if not os.path.isfile(raw) or os.path.getsize(raw) < (1 << 30):
        raise SetupError('DECRYPT', 'The game couldn\'t be decrypted',
                         'prod.keys must be your console\'s and hold the keys for this game (dump them again with a '
                         'current Lockpick_RCM); an NSP also needs its ticket.')
    upath, ut = upd
    uname, uoff, usize = ut['program']
    unca = os.path.join(stage, 'update.nca')
    say('step', 'Applying the update from ' + os.path.basename(upath))
    copy_range(upath, uoff, usize, unca)
    tk = [f'--titlekey={ut["titlekey"]}'] if ut['titlekey'] else []
    hactool_watched(hac, romfs, bsize, 0.3, 0.6, *tk, '-t', 'nca', f'--baseromfs={raw}', f'--exefsdir={exefs}',
                    f'--romfsdir={romfs}', unca, what='Applying the update')
    os.remove(unca)
    os.remove(raw)
    if not os.path.isfile(os.path.join(exefs, 'main')) or not os.path.isdir(romfs):
        raise SetupError('DECRYPT', 'The update couldn\'t be decrypted or applied',
                         'prod.keys must hold the keys for the 3.0.3 update (dump them again with a current '
                         'Lockpick_RCM), and the update NSP must be complete, with its ticket.')
    return {'exefs': exefs, 'romfs': romfs}


# ------------------------------------------------------------------ sources -> layout
CONTAINER_EXT = ('.nsp', '.xci')


def scan_sources(sources, out=None):
    """Extracted ExeFS / RomFS folders and NSP / XCI files in what the player chose (files, or folders searched a few
    levels down). Setup's own output (out) is skipped: a chosen folder that holds it (on Windows the program's folder
    is the data folder) would otherwise have its last setup copied and linked onto itself."""
    exefs, romfs, containers = [], [], []
    own = os.path.realpath(out) if out else None

    def walk(d, depth):
        if own and os.path.realpath(d) == own:
            return
        if is_exefs(d):
            exefs.append(d)
        if is_romfs(d):
            romfs.append(d)
            return
        if depth >= 4:
            return
        try:
            names = sorted(os.listdir(d))
        except OSError:
            return
        for n in names:
            if n.startswith('.'):
                continue
            p = os.path.join(d, n)
            if os.path.isdir(p):
                walk(p, depth + 1)
            elif n.lower().endswith(CONTAINER_EXT):
                containers.append(p)
            elif n.lower().endswith(('.nsz', '.xcz')):
                raise SetupError('COMPRESSED', f'{n} is compressed (NSZ / XCZ)',
                                 'Decompress it to an NSP / XCI first (e.g. with nsz -D).')

    for s in sources:
        if os.path.isfile(s):
            if s.lower().endswith(CONTAINER_EXT):
                containers.append(s)
            elif s.lower().endswith(('.nsz', '.xcz')):
                raise SetupError('COMPRESSED', f'{os.path.basename(s)} is compressed (NSZ / XCZ)',
                                 'Decompress it to an NSP / XCI first (e.g. with nsz -D).')
            else:
                raise SetupError('NOT_CONTAINER', f'{os.path.basename(s)} is not an NSP, XCI or folder',
                                 'Choose the game\'s NSP or XCI file, or the folder of its extracted files.')
        elif os.path.isdir(s):
            walk(s, 0)
        else:
            raise SetupError('NOT_FOUND', f'{s} doesn\'t exist', 'Choose the game files again.')
    return exefs, romfs, containers


def setup_from_sources(sources, out, keys, hactool, check_hashes=True):
    say('step', 'Looking at the game files')
    exefs, romfs, containers = scan_sources(sources, out)
    good = [d for d in exefs if exefs_version(d) == '3.0.3']
    stage = os.path.join(out, '.extract')
    extracted = None
    if containers and not (good and romfs):
        hac = Hactool(hactool, keys)
        extracted = extract_containers(containers, hac, stage)
        exefs.append(extracted['exefs'])
        good = [d for d in exefs if exefs_version(d) == '3.0.3']
    if not good:
        vers = sorted({exefs_version(d) or 'unknown' for d in exefs})
        if exefs and vers != ['unknown']:
            raise SetupError('WRONG_VERSION', f'These game files are version {", ".join(vers)}',
                             'The recomp needs version 3.0.3: add the 3.0.3 update (its NSP, or its extracted ExeFS '
                             'and RomFS).')
        raise SetupError('NO_EXEFS', 'No game code (ExeFS) found in the chosen files',
                         'Choose the game\'s NSP / XCI with its 3.0.3 update NSP, or the folders of the extracted '
                         'ExeFS (main, sdk, rtld, main.npdm) and RomFS.')
    say('step', 'Checking the game version')
    say('progress', '0.62')
    ex = good[0]
    copy_exefs(ex, os.path.join(out, 'exefs'), check_hashes)
    say('found', 'ExeFS: Super Mario Maker 2 3.0.3 (build ids and hashes match)')

    say('step', 'Setting up the game data')
    dst = os.path.join(out, 'romfs')
    if extracted:
        if os.path.lexists(dst):
            shutil.rmtree(dst) if os.path.isdir(dst) and not os.path.islink(dst) else os.remove(dst)
        os.replace(extracted['romfs'], dst)
        shutil.rmtree(stage, ignore_errors=True)
        layers = ['(extracted, patched by the update)']
    else:
        if not romfs:
            raise SetupError('NO_ROMFS', 'No game data (RomFS) found in the chosen files',
                             'Also choose the folder of the extracted RomFS (it holds Pack, System, Layout, ...).')
        # the update's RomFS (next to the 3.0.3 ExeFS) goes last, over the base game's
        parent = os.path.dirname(os.path.realpath(ex))
        layers = sorted(romfs, key=lambda d: os.path.dirname(os.path.realpath(d)) == parent)
        counts = {'linked': 0, 'replaced': 0, 'same': 0}
        for i, d in enumerate(layers):
            link_tree(d, dst, counts)
            say('progress', f'{0.62 + 0.36 * (i + 1) / len(layers):.3f}')
    n = sum(1 for _, _, fs in os.walk(dst) for _ in fs)
    if n < 1000:
        raise SetupError('ROMFS_SMALL', f'The game data has only {n} files',
                         'The RomFS looks incomplete: extract it again (the whole RomFS of the game with the update).')
    say('found', f'RomFS: {n} files')
    with open(os.path.join(out, 'setup.json'), 'w', encoding='utf-8') as f:
        # (sources as given: the launcher compares them with what the player chose)
        json.dump({'version': '3.0.3', 'sources': list(sources), 'exefs': os.path.abspath(ex),
                   'romfs': [os.path.abspath(d) for d in layers] if not extracted else ['extracted'],
                   'build_ids': {m: build_id(os.path.join(out, 'exefs', m)) for m in ('main', 'sdk')},
                   'time': time.strftime('%Y-%m-%dT%H:%M:%S')}, f, indent=1, ensure_ascii=False)
    say('progress', '1')
    say('done')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base-romfs')
    ap.add_argument('--update-exefs')
    ap.add_argument('--update-romfs')
    ap.add_argument('--source', action='append', default=[])
    ap.add_argument('--keys')
    ap.add_argument('--hactool')
    ap.add_argument('--no-hash-check', action='store_true')  # (development: a modified ExeFS)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    if a.source:
        try:
            setup_from_sources(a.source, a.out, a.keys, a.hactool, not a.no_hash_check)
        except SetupError as e:
            say('error', f'{e.code}\t{e.what}\t{e.hint}')
            sys.exit(1)
        except OSError as e:
            code = 'DISK' if getattr(e, 'errno', 0) == 28 else 'IO'
            say('error', f'{code}\t{masked(str(e))}\tCheck the disk and the files, then try again.')
            sys.exit(1)
        return
    if not (a.base_romfs and a.update_exefs and a.update_romfs):
        ap.error('pass --source, or --base-romfs, --update-exefs and --update-romfs')
    for d in (a.base_romfs, a.update_exefs, a.update_romfs):
        if not os.path.isdir(d):
            sys.exit(f'setup: {d} is not a folder (pass DUMP_BASE_ROMFS / DUMP_UPDATE_EXEFS / DUMP_UPDATE_ROMFS)')
    try:
        copy_exefs(a.update_exefs, os.path.join(a.out, 'exefs'), not a.no_hash_check)
    except SetupError as e:
        sys.exit(f'setup: {e.what}. {e.hint}')
    romfs = os.path.join(a.out, 'romfs')
    counts = {'linked': 0, 'replaced': 0, 'same': 0}
    link_tree(a.base_romfs, romfs, counts)
    base = dict(counts)
    link_tree(a.update_romfs, romfs, counts)
    n = sum(1 for _, _, fs in os.walk(romfs) for _ in fs)
    print(f'setup: romfs ok: {n} files ({base["linked"] + base["same"]} from the base game, '
          f'{counts["replaced"] - base["replaced"]} replaced by the update)')


if __name__ == '__main__':
    main()
