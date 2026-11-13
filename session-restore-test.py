#!/usr/bin/env python3
"""Isolated Wine GUI session test. Run: python3 session-restore-test.py [EXE [PDF]].
Retains a private /tmp directory with logs and timed settings snapshots. Exit 0 only
when window and tab restoration are observed, 1 on failure, 2 if unavailable.
Requires wine, xvfb-run, xauth, xdotool and Python 3. No existing prefix is used.
"""
import os, pathlib, shutil, subprocess, sys, tempfile, time

BASE = pathlib.Path(__file__).resolve().parent
EXE = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else BASE / 'tree/out/rel64/SumatraPDF-static.exe').resolve()
PDF = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else BASE.parent / 'sumatrapdf/docs/test/zlib.3.pdf').resolve()

def cmd(*args, **kw):
    return subprocess.run(args, check=True, **kw)

def main():
    if len(sys.argv) > 3:
        print(__doc__); return 2
    for tool in ('wine', 'xvfb-run', 'xauth', 'xdotool'):
        if not shutil.which(tool):
            print('UNVERIFIED: missing ' + tool); return 2
    if not EXE.is_file() or not PDF.is_file():
        print('UNVERIFIED: EXE or PDF missing'); return 2
    root = pathlib.Path(tempfile.mkdtemp(prefix='sumatra-session-'))
    os.chmod(root, 0o700)
    app = root / 'app'; app.mkdir()
    shutil.copy2(EXE, app / 'SumatraPDF.exe')
    for name in ('alpha', 'bravo', 'charlie', 'delta'):
        shutil.copy2(PDF, app / (name + '.pdf'))
    (app / 'SumatraPDF-settings.txt').write_text('''RestoreSession = true
RememberOpenedFiles = true
UseTabs = true
ReuseInstance = true
CheckForUpdates = false
''', encoding='utf-8')
    print('Artifacts:', root, flush=True)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(root / 'prefix'), WINEDEBUG='-all', SUMATRA_SESSION_WORKER='1', SUMATRA_SESSION_ROOT=str(root))
    # xvfb-run owns its X server; it only stops that server on exit.
    return subprocess.call(['xvfb-run', '-a', sys.executable, str(pathlib.Path(__file__).resolve())], env=env)

def worker():
    root = pathlib.Path(os.environ['SUMATRA_SESSION_ROOT']); app = root / 'app'
    exe = str(app / 'SumatraPDF.exe'); settings = app / 'SumatraPDF-settings.txt'
    log = open(root / 'wine.log', 'ab', buffering=0)
    processes = []
    def launch(*args):
        p = subprocess.Popen(['wine', exe, *args], cwd=app, stdout=log, stderr=log)
        processes.append(p); return p
    def windows():
        p = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', '(?i)sumatrapdf|alpha\.pdf|bravo\.pdf|charlie\.pdf|delta\.pdf'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        return p.stdout.split() if p.returncode == 0 else []
    def wait_for(predicate, seconds, label):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            result = predicate()
            if result: return result
            time.sleep(.5)
        raise RuntimeError('timeout: ' + label)
    def snapshot(name):
        if not settings.exists(): raise RuntimeError('settings file missing')
        text = settings.read_text(encoding='utf-8-sig', errors='replace')
        (root / (name + '.txt')).write_text(text, encoding='utf-8')
        return text
    def session(text):
        # Parse bracket nesting so FileStates entries cannot satisfy session assertions.
        stack = []; groups = []; window = None
        for line in text.splitlines():
            s = line.strip()
            if s.endswith('['):
                key = s[:-1].strip(); stack.append(key)
                if stack == ['SessionData', '']: window = []; groups.append(window)
            elif s == ']':
                if stack: stack.pop()
            elif window is not None and len(stack) >= 2 and stack[0] == 'SessionData' and s.startswith('FilePath ='):
                window.append(s.split('=', 1)[1].strip().replace('\\', '/').split('/')[-1].lower())
        return groups
    def assert_groups(text, phase):
        groups = session(text)
        expected = {frozenset(('alpha.pdf','bravo.pdf')), frozenset(('charlie.pdf','delta.pdf'))}
        if len(groups) != 2 or {frozenset(x) for x in groups} != expected or any(len(x) != 2 for x in groups):
            raise RuntimeError(phase + ': unexpected SessionData ' + repr(groups))
    def exit_app():
        ids = windows()
        if ids:
            cmd('xdotool', 'windowfocus', '--sync', ids[0])
            cmd('xdotool', 'key', '--clearmodifiers', 'ctrl+q')
        wait_for(lambda: not windows(), 20, 'application exit')
    try:
        launch('alpha.pdf', 'bravo.pdf')
        wait_for(lambda: len(windows()) == 1, 90, 'first window')
        time.sleep(3)
        launch('-reuse-instance', '-new-window-tabs', 'charlie.pdf', 'delta.pdf')
        wait_for(lambda: len(windows()) == 2, 30, 'second window')
        for i in range(3):
            time.sleep(3)
            text = snapshot('live-%d' % (i+1))
        assert_groups(text, 'live snapshot')
        exit_app()
        for p in processes:
            try: p.wait(timeout=20)
            except subprocess.TimeoutExpired: raise RuntimeError('launched Wine process did not exit')
        assert_groups(snapshot('closed'), 'closed snapshot')
        processes.clear()
        launch()
        ids = wait_for(lambda: windows() if len(windows()) == 2 else None, 45, 'restored windows')
        # Check each visible window by switching tabs and observing both titles.
        titles = []
        for wid in ids:
            cmd('xdotool', 'windowfocus', '--sync', wid)
            time.sleep(1)
            pair = []
            for _ in range(2):
                pair.append(subprocess.check_output(['xdotool', 'getwindowname', wid], universal_newlines=True).strip().lower())
                cmd('xdotool', 'key', '--clearmodifiers', 'ctrl+Tab'); time.sleep(1)
            titles.append(pair)
        (root / 'restored-titles.txt').write_text(repr(titles) + chr(10))
        for expected in (('alpha.pdf','bravo.pdf'), ('charlie.pdf','delta.pdf')):
            if not any(all(any(name in title for title in pair) for name in expected) for pair in titles):
                raise RuntimeError('restored tab titles not observed: ' + repr(titles))
        time.sleep(3); assert_groups(snapshot('restored'), 'restored snapshot')
        exit_app()
        for p in processes: p.wait(timeout=20)
        print('PASS: two windows, two tabs each, snapshots, clean exit and GUI restoration', flush=True)
        return 0
    except (RuntimeError, OSError, subprocess.SubprocessError) as e:
        print('FAIL / UNVERIFIED:', e, '; artifacts:', root, flush=True)
        try: exit_app()
        except Exception as cleanup: print('Cleanup unverified:', cleanup, flush=True)
        return 1
    finally:
        log.close()

if __name__ == '__main__':
    sys.exit(worker() if os.environ.get('SUMATRA_SESSION_WORKER') == '1' else main())
