"""Check the native log viewer, then read real original-game logs in a stopped private instance."""

import argparse
import ctypes as c
import datetime
import json
from pathlib import Path
import platform
import shutil
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import external_output, sha256, BUILD
from probe_core_protocols import channel, relay, stun
from gui_support import user, windows, wait_for, click, ready, title, screenshot, responsive, hidden_startup
from gui_support import rows, select, choose_log_source

from gui_support import guest_root

ROOT = Path(__file__).resolve().parents[1]






def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--instance', type=Path, required=True)
    parser.add_argument('--gui', type=Path, required=True)
    args = parser.parse_args()
    instance, gui = external_output(args.instance).resolve(strict=True), args.gui.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    probe = BUILD / 'probes' / stamp / 'gui-logs'
    probe.mkdir(parents=True)
    receipt = {'recorded_utc': stamp, 'os': platform.platform(), 'gui_sha256': sha256(gui),
               'script_sha256': sha256(Path(__file__)), 'instance': str(instance.relative_to(BUILD)),
               'scope': 'Private viewer fixtures and actual original game/peer/MySQL logs; no client gameplay acceptance.',
               'checks': {}, 'forced_cleanup': False}
    log_dir = guest_root(instance) / 'home/neople/game/log'
    if log_dir.exists():
        shutil.move(str(log_dir), str(probe / 'prior-game-logs'))
    (log_dir / 'siroco11').mkdir(parents=True)
    fixture = log_dir / 'gui-viewer.fixture.log'
    fixture.write_text('原文标记一\n原文标记二\n', encoding='utf-8')
    process = subprocess.Popen([str(gui), '--instance', str(instance)], startupinfo=hidden_startup())
    window = None
    checks = receipt['checks']
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, 'DNFNativeController')), None), 'window')
        wait_for(lambda: user.SendMessageW(user.GetDlgItem(window, 220), 0x18E, 0, 0) == 0, 'initial selected service visible')
        checks['initial_selected_service_visible'] = True
        game = next(i for i, row in enumerate(rows(window)) if row.startswith('game'))
        select(window, game)
        choose_log_source(window, fixture.name)
        output = user.GetDlgItem(window, 206)
        wait_for(lambda: title(output) == '原文标记一\r\n原文标记二\r\n', 'unaltered UTF-8 text')
        checks['original_text_without_controller_prefix'] = True
        with fixture.open('ab') as file:
            file.write('自动追加\n'.encode('utf-8'))
        wait_for(lambda: '自动追加' in title(output), 'live append', 8)
        checks['automatic_append'] = True
        fixture.write_bytes('简体编码原文\n'.encode('gbk'))
        encoding = user.GetDlgItem(window, 241)
        user.SendMessageW(encoding, 0x14E, 2, 0)
        user.SendMessageW(window, 0x111, 241 | (1 << 16), encoding)
        wait_for(lambda: title(output) == '简体编码原文\r\n', 'explicit codepage')
        checks['explicit_gbk_encoding_and_truncation'] = True
        user.SendMessageW(encoding, 0x14E, 0, 0)
        user.SendMessageW(window, 0x111, 241 | (1 << 16), encoding)
        rotated = fixture.with_suffix('.previous')
        fixture.rename(rotated)
        fixture.write_text('replacement file\n', encoding='ascii')
        wait_for(lambda: title(output) == 'replacement file\r\n', 'replacement at same filename', 8)
        checks['rotated_file_reopened'] = True
        fixture.write_bytes(b'old line\n' * 150000 + b'LATEST ORIGINAL LINE\n')
        wait_for(lambda: 'LATEST ORIGINAL LINE' in title(output), 'bounded tail')
        assert len(title(output)) < 300000
        checks['large_log_tail_bounded'] = True
        click(window, 242)
        user.SendMessageW(output, 0xB1, 0, 10)
        user.SendMessageW(output, 0xB6, 0, 30)
        first_line = user.SendMessageW(output, 0xCE, 0, 0)
        assert first_line > 0
        with fixture.open('ab') as file:
            file.write(b'NEWEST LINE\n')
        wait_for(lambda: 'NEWEST LINE' in title(output), 'unfollowed append')
        assert user.SendMessageW(output, 0xCE, 0, 0) == first_line
        checks['unfollowed_scroll_preserved'] = True
        click(window, 242)
        fixture.unlink(); rotated.unlink()
        click(window, 243)
        click(window, 223)
        ready(window, '已启动默认服务进程', 90)
        select(window, game)
        if '进程运行中' not in rows(window)[game]:
            click(window, 221)
            ready(window, 'game：进程运行中', 60)
        started = time.monotonic()
        def game_ready():
            if '已退出' in rows(window)[game]:
                raise RuntimeError('Original game exited before its ready marker')
            return next((path for path in log_dir.rglob('*.init') if b'server has been started successfully' in path.read_bytes()), None)
        original = wait_for(game_ready, 'original game ready marker', 150)
        checks['original_game_natural_start_seconds_after_launch'] = round(time.monotonic() - started, 3)
        user.ShowWindow(window,4)  # Native caret scrolling requires a visible window.
        choose_log_source(window, original.name)
        wait_for(lambda: 'server has been started successfully' in title(output), 'original application init log')
        assert user.SendMessageW(output, 0xCE, 0, 0) > 0
        checks['follow_scrolls_to_latest_lines'] = True
        expected = original.read_text(encoding='utf-8', errors='replace').splitlines()[-1]
        assert expected in title(output)
        checks['actual_application_init_log_visible'] = True
        screenshot(window, probe / 'original-game-log.png')
        user.ShowWindow(window,0)
        checks['window_response_ms'] = responsive(window)
        choose_log_source(window, '标准输出')
        wait_for(lambda: len(title(output)) > 20, 'actual stdout')
        checks['actual_stdout_visible'] = True
        choose_log_source(window, '标准错误 / 宿主诊断')
        wait_for(lambda: 'ELF dependency private snapshot' in title(output), 'actual stderr')
        checks['actual_stderr_visible'] = True
        choose_log_source(window, 'MySQL 原生日志')
        wait_for(lambda: 'ready for connections' in title(output), 'actual MySQL log')
        checks['actual_mysql_log_visible'] = True
        checks['peer_protocols'] = {'channel': channel(7001), 'relay_and_real_monitor_rejection': relay(True), 'stun': stun()}
        choose_log_source(window, original.name)
        user.SetWindowPos.argtypes = [c.c_void_p, c.c_void_p, c.c_int, c.c_int, c.c_int, c.c_int, c.c_uint]
        user.SetWindowPos(window, None, 0, 0, 1040, 760, 0x16)
        time.sleep(0.3)
        screenshot(window, probe / 'minimum-window.png')
        checks['minimum_window_captured'] = True
        click(window, 224)
        ready(window, '服务进程已退出，数据库已停止', 180)
        select(window, game)
        choose_log_source(window, '标准错误 / 宿主诊断')
        wait_for(lambda: 'guest exit status=0' in title(output), 'actual stopped guest log')
        checks['logs_readable_after_normal_stop'] = True
        assert '代码 0' in rows(window)[game]
        checks['game_normal_exit'] = True
        screenshot(window, probe / 'after-stop.png')
        user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=90)
        assert process.returncode == 0
        print('Native GUI original logs, live/encoding/rotation/tail/scroll checks passed', flush=True)
    except Exception as error:
        receipt['error'] = type(error).__name__ + ': ' + str(error)
        if window:
            screenshot(window, probe / 'failure.private.png')
        raise
    finally:
        if process.poll() is None:
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                receipt['forced_cleanup'] = True
                subprocess.run(['taskkill.exe', '/PID', str(process.pid), '/T', '/F'], capture_output=True, timeout=15)
        receipt['exit_code'] = process.returncode
        for path in log_dir.glob('gui-viewer.fixture.*'):
            path.unlink()
        (probe / 'receipt.json').write_text(json.dumps(receipt, ensure_ascii=True, indent=2) + '\n', encoding='utf-8')
        print('GUI log receipt:', probe / 'receipt.json', flush=True)


if __name__ == '__main__':
    main()
