#!/usr/bin/env python3
# Behavioral checks for the SDL2 host runner. Not a source-text grep.
# SPDX-License-Identifier: MIT
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

import threading

def run(exe, args, *, input_bytes=None, env=None, cwd=None):
    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    if env:
        merged.update(env)
    return subprocess.run(
        [exe, *args],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
        cwd=cwd,
        timeout=30,
        check=False,
    )


def show(proc):
    sys.stderr.write(proc.stdout.decode("utf-8", "replace"))
    sys.stderr.write(proc.stderr.decode("utf-8", "replace"))


def expect_ok(proc):
    if proc.returncode != 0:
        show(proc)
        raise SystemExit(f"exit {proc.returncode}")


def checkerboard():
    width, height = 320, 240
    out = bytearray(width * 2 * height)
    for y in range(height):
        for x in range(width):
            pixel = (
                (0xF800 if (x & 32) else 0)
                | (0x07E0 if (y & 32) else 0)
                | 0x001F
            )
            i = (y * width + x) * 2
            out[i] = (pixel >> 8) & 0xFF
            out[i + 1] = pixel & 0xFF
    return bytes(out)


def tone(frames):
    out = bytearray()
    for i in range(frames):
        phase = i % 50
        value = 12000 if phase < 25 else -12000
        out += struct.pack("<hh", value, value)
    return bytes(out)


def check_hello(exe):
    proc = run(exe, ["--headless"])
    expect_ok(proc)
    if b"hello zzapp" not in proc.stdout:
        show(proc)
        raise SystemExit("hello text missing")


def check_console(exe):
    proc = run(exe, ["--headless", "--", "one"], input_bytes=b"Hello, zzapp!\n")
    expect_ok(proc)
    text = proc.stdout.decode("utf-8", "replace")
    err = proc.stderr.decode("utf-8", "replace")
    if "console: 1 argument(s) [one]" not in text:
        show(proc)
        raise SystemExit("console banner")
    if "HELLO, ZZAPP!\n" not in text:
        show(proc)
        raise SystemExit("console echo")
    if "EOF after 1 line" not in text:
        show(proc)
        raise SystemExit("console eof")
    if "console: stderr works" not in err:
        show(proc)
        raise SystemExit("console stderr")


def check_filter(exe):
    with tempfile.TemporaryDirectory() as root:
        data = b"zzapp-host filter\n"
        with open(os.path.join(root, "in.bin"), "wb") as fh:
            fh.write(data)
        proc = run(exe, ["--root", root, "--", "in.bin", "out.bin"])
        expect_ok(proc)
        with open(os.path.join(root, "out.bin"), "rb") as fh:
            got = fh.read()
        want = hashlib.sha256(data).digest()
        if got != want:
            show(proc)
            raise SystemExit(f"filter digest {got.hex()} != {want.hex()}")


def check_echo(exe):
    messages = [
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    ]
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "msg.txt")
        out = os.path.join(root, "msg.out")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("\n".join(messages + ["stop"]) + "\n")
        proc = run(
            exe,
            ["--headless", "--msg-script", script, "--msg-out", out],
        )
        expect_ok(proc)
        with open(out, "r", encoding="ascii") as fh:
            got = [line.strip() for line in fh if line.strip()]
        if got != messages:
            show(proc)
            raise SystemExit(f"echo messages {got}")


def check_msgflood(exe):
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "msg.txt")
        out = os.path.join(root, "msg.out")
        with open(script, "w", encoding="ascii") as fh:
            fh.write('"ping"\nstop\n')
        proc = run(
            exe,
            ["--headless", "--msg-script", script, "--msg-out", out],
        )
        expect_ok(proc)
        with open(out, "r", encoding="ascii") as fh:
            got = [line for line in fh if line.strip()]
        if len(got) != 100:
            show(proc)
            raise SystemExit(f"msgflood recorded {len(got)} of 100 messages")


def check_conshort(exe):
    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    proc = subprocess.Popen(
        [exe, "--headless"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
    )
    try:
        proc.stdin.write(b"hello\n")
        proc.stdin.flush()  # the pipe stays open: a full-length read must not block
        out = []
        reader = threading.Thread(
            target=lambda: out.append(proc.stdout.readline()), daemon=True
        )
        reader.start()
        reader.join(10)
        if not out or b"got 6: hello" not in out[0]:
            raise SystemExit(f"console read blocked or wrong: {out!r}")
        rc = proc.wait(timeout=10)
        if rc != 0:
            raise SystemExit(f"exit {rc}")
    finally:
        proc.stdin.close()
        proc.kill()
        proc.wait()


def check_mailbox(exe):
    proc = run(exe, ["--headless"])
    expect_ok(proc)


def check_constop(exe):
    if os.name != "posix":
        return
    import signal
    import time

    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    proc = subprocess.Popen(
        [exe, "--headless"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
    )
    try:
        time.sleep(0.5)  # let the app block in zz_console_read on the empty pipe
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise SystemExit("console read not interrupted by SIGTERM")
        if rc != 1:
            raise SystemExit(f"expected the stop result 1, got {rc}")
    finally:
        proc.stdin.close()
        proc.kill()
        proc.wait()


def check_pixels(exe):
    want = checkerboard()
    with tempfile.TemporaryDirectory() as root:
        dumps = []
        for name in ("a.bin", "b.bin"):
            path = os.path.join(root, name)
            proc = run(
                exe,
                ["--headless", "--frames", "2", "--dump-front", path],
            )
            expect_ok(proc)
            err = proc.stderr.decode("utf-8", "replace")
            if "flips=2" not in err:
                show(proc)
                raise SystemExit("pixels flip count")
            with open(path, "rb") as fh:
                dumps.append(fh.read())
        if dumps[0] != dumps[1]:
            raise SystemExit("pixels dumps differ")
        if dumps[0] != want:
            raise SystemExit(
                f"pixels bytes {len(dumps[0])} != expected {len(want)}"
            )


def check_tone(exe):
    with tempfile.TemporaryDirectory() as root:
        path = os.path.join(root, "tone.pcm")
        proc = run(
            exe,
            [
                "--headless",
                "--audio-null",
                "--audio-frames",
                "256",
                "--audio-dump",
                path,
            ],
        )
        expect_ok(proc)
        err = proc.stderr.decode("utf-8", "replace")
        if "audio_frames=256" not in err:
            show(proc)
            raise SystemExit("tone frame count")
        with open(path, "rb") as fh:
            got = fh.read()
        if got != tone(256):
            raise SystemExit("tone samples")


def check_keys(exe):
    proc = run(exe, ["--headless"], env={"ZZAPP_HOST_SELFTEST": "1"})
    expect_ok(proc)


def prepare_root(root):
    os.makedirs(os.path.join(root, "foo"))
    with open(os.path.join(root, "ok.txt"), "w", encoding="ascii") as fh:
        fh.write("ok\n")
    with open(os.path.join(root, "bar.txt"), "w", encoding="ascii") as fh:
        fh.write("bar\n")
    with open(os.path.join(root, "empty.txt"), "wb") as fh:
        fh.write(b"")
    with open(os.path.join(root, "big.bin"), "wb") as fh:
        fh.seek(16 * 1024 * 1024)
        fh.write(b"x")


def check_contract(exe):
    with tempfile.TemporaryDirectory() as root:
        prepare_root(root)
        proc = run(exe, ["--headless", "--root", root, "--", "reject"])
        expect_ok(proc)
        proc = run(exe, ["--headless", "--", "display"])
        expect_ok(proc)
        proc = run(exe, ["--", "services"])
        expect_ok(proc)
        script = os.path.join(root, "input.txt")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("key down 0x22 8\nbreak 4\n")
        proc = run(exe, ["--headless", "--input", script, "--", "input"])
        expect_ok(proc)
    with tempfile.TemporaryDirectory() as cwd:
        with open(os.path.join(cwd, "vol:data"), "wb") as fh:
            fh.write(b"abs-ok")
        proc = run(exe, ["--abs", "--", "abs"], cwd=cwd)
        expect_ok(proc)


CHECKS = {
    "hello": check_hello,
    "console": check_console,
    "filter": check_filter,
    "echo": check_echo,
    "pixels": check_pixels,
    "tone": check_tone,
    "keys": check_keys,
    "contract": check_contract,
    "msgflood": check_msgflood,
    "conshort": check_conshort,
    "mailbox": check_mailbox,
    "constop": check_constop,
}


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in CHECKS:
        raise SystemExit("usage: check_runner.py <test> <exe>")
    CHECKS[sys.argv[1]](sys.argv[2])


if __name__ == "__main__":
    main()
