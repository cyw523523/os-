#!/usr/bin/env python3
"""Check that both sides of the WAL commit point survive a QEMU reset."""
import re
import selectors
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
from time import monotonic


ROOT = Path(__file__).resolve().parent.parent
BLOCK = 1024
COMMITTED = {3, 4, 5, 7}
PENDING_REPLAY = {3, 4, 7}


class Guest:
    def __init__(self, image, log, stop_on_reset=False):
        self.log = log
        args = [
            'qemu-system-riscv64', '-machine', 'virt', '-bios', 'none',
            '-kernel', 'kernel/kernel', '-m', '128M', '-smp', '1', '-nographic',
            '-global', 'virtio-mmio.force-legacy=false',
            '-drive', f'file={image},if=none,format=raw,id=x0,cache=directsync',
            '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0',
        ]
        if stop_on_reset:
            args.append('-no-reboot')
        self.proc = subprocess.Popen(
            args, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0,
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        self.pending = b''

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.selector.close()
        self.proc.stdin.close()
        self.proc.stdout.close()

    def send(self, value):
        self.proc.stdin.write((value + '\n').encode())

    def expect(self, value, timeout=10):
        target = value.encode()
        end = monotonic() + timeout
        while target not in self.pending and monotonic() < end:
            if not self.selector.select(max(0, end - monotonic())):
                break
            data = self.proc.stdout.read(4096)
            if not data:
                break
            self.log.write(data)
            self.log.flush()
            self.pending += data
        if target not in self.pending:
            raise AssertionError(f'missing {value!r}: {self.pending[-1000:]!r}')
        pos = self.pending.index(target) + len(target)
        matched, self.pending = self.pending[:pos], self.pending[pos:]
        if re.search(rb'FAIL|panic:|kernel trap', matched):
            raise AssertionError(matched[-1000:])
        return matched


def wal_header(image):
    with image.open('rb') as data:
        data.seek(2 * BLOCK)
        return struct.unpack('<I', data.read(4))[0]


def recover(image, log, expected, replay):
    with Guest(image, log) as guest:
        boot = guest.expect('sh> ')
        assert (b'fs: replay ' in boot) == replay, boot
        guest.send('fsverify')
        report = guest.expect('FSVERIFY PASS')
        assert expected.encode() in report, report
    assert wal_header(image) == 0


def occupied_blocks(image):
    with image.open('rb') as data:
        data.seek(BLOCK)
        *_, bitmap = struct.unpack('<8I', data.read(32))
        data.seek(bitmap * BLOCK)
        return data.read(BLOCK)


def run_case(base, image, log_dir, stage, mode):
    shutil.copyfile(base, image)
    tag = f'{mode}{stage}'
    with (log_dir / f'{tag}.log').open('wb') as log:
        with Guest(image, log, stop_on_reset=True) as guest:
            guest.expect('sh> ')
            guest.send('fscrash')
            guest.expect('o=orphan):')
            guest.send(f'{stage}{mode}')
            guest.expect(f'crash: stage {stage}')
            assert guest.proc.wait(10) == 0, tag

        committed = stage in COMMITTED
        replay = stage in PENDING_REPLAY
        assert (wal_header(image) > 0) == replay, tag
        expected = {
            'c': ('CREATED absent', 'CREATED empty-file'),
            'w': ('CONTENT OLD', 'CONTENT NEW-V2'),
            'o': ('ORPHAN present', 'ORPHAN absent'),
        }[mode][committed]
        recover(image, log, expected, replay)
        recover(image, log, expected, False)
        if mode == 'o' and committed:
            assert occupied_blocks(image) == occupied_blocks(base), 'orphan block leaked'
    print(f'PASS {tag} {expected} replay={int(replay)} second_boot=pass', flush=True)


def main():
    subprocess.run(['make', '-s', '-j4'], cwd=ROOT, check=True)
    log_dir = ROOT / 'support' / 'lab6-logs'
    log_dir.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp) / 'base.img'
        image = Path(tmp) / 'case.img'
        programs = sorted(
            elf.stem for elf in (ROOT / 'user-flat').glob('*.elf')
            if (ROOT / 'user-flat/image' / elf.stem).is_file()
        )
        subprocess.run([ROOT / 'mkfs/mkfs', base, *programs],
                       cwd=ROOT / 'user-flat/image', check=True)
        for mode in ('c', 'w'):
            for stage in range(8):
                run_case(base, image, log_dir, stage, mode)
        run_case(base, image, log_dir, 4, 'o')
    print('LAB6 CRASH MATRIX PASS (17 cases)')


if __name__ == '__main__':
    main()
