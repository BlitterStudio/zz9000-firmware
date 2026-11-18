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


def fnv1a32(data):
    h = 2166136261
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


def check_freestanding(exe):
    # The template fills 3 MiB from an xorshift32 stream and hashes it. This
    # recomputes the hash independently of the C code.
    seed = 0x2545F491
    data = bytearray(3 << 20)
    for i in range(len(data)):
        seed ^= (seed << 13) & 0xFFFFFFFF
        seed ^= seed >> 17
        seed ^= (seed << 5) & 0xFFFFFFFF
        data[i] = seed & 0xFF
    want = "freestanding ok: hash=%08x " % fnv1a32(data)
    proc = run(exe, ["--headless"])
    expect_ok(proc)
    out = proc.stdout.decode("utf-8", "replace")
    if want not in out or "FAIL" in out:
        show(proc)
        raise SystemExit("freestanding line %r missing" % want)


def ring_mix(h):
    h &= 0xFFFFFFFF
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16
    return h


def ring_ping(ident):
    return struct.pack("<8I", 5, ident, 0x11110000 + ident, 0x22, 0, 0, 0, 0)


def ring_ctrl(ident, mode):
    return struct.pack("<8I", 7, ident, mode, 0, 0, 0, 0, 0)


def ring_run(exe, script_messages, args, stop=False):
    """Runs ringflood with injected messages and returns what it sent."""
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "msg.txt")
        out = os.path.join(root, "msg.out")
        with open(script, "w", encoding="ascii") as fh:
            for message in script_messages:
                fh.write(message.hex() + "\n")
            if stop:
                fh.write("stop\n")
        argv = ["--headless", "--msg-script", script, "--msg-out", out]
        if args:
            argv += ["--"] + args
        proc = run(exe, argv)
        expect_ok(proc)
        with open(out, "r", encoding="ascii") as fh:
            lines = [line.strip() for line in fh if line.strip()]
    return [struct.unpack("<8I", bytes.fromhex(line)) for line in lines]


def check_ringflood(exe):
    # Known answers shared with sdk/tests/app_ringtest_logic_test.c: pattern
    # word i of message seq is ring_mix(seq * 4 + i).
    vectors = {
        1: (0x249CB285, 0xCC0D53CD, 0x5CEB4D08, 0x18C9AEC4),
        2: (0x4939650B, 0xC27C2913, 0xE9250490, 0x93A0FE04),
        1000: (0x1F0A4224, 0x9F34547F, 0xB764BC10, 0x4F4FFC5F),
        0xFFFFFFFE: (0xF94C9009, 0x0CC81F63, 0xC24094C8, 0x937F414E),
    }
    for seq, words in vectors.items():
        got = tuple(ring_mix((seq * 4 + i) & 0xFFFFFFFF) for i in range(4))
        if got != words:
            raise SystemExit(f"pattern vector for seq {seq}: {got}")
    # Run A: the app starts quiet and answers pings, floods after CTRL, and
    # leaves the flood after `count` DATA messages.
    count = 500
    msgs = ring_run(exe, [ring_ping(7), ring_ping(8), ring_ping(9),
                          ring_ctrl(1, 1)], [str(count)])
    info = msgs[0]
    if info[0] != 1 or info[1] != 1 or info[3] != 4096 or info[2] == 0xFFFFFFFF:
        raise SystemExit(f"INFO message {info}")
    kinds = [m[0] for m in msgs]
    if kinds[:5] != [1, 6, 6, 6, 8]:
        raise SystemExit(f"quiet start sent {kinds[:5]}, not INFO, 3 PONG, ACK")
    pongs = [m for m in msgs if m[0] == 6]
    if [m[1] for m in pongs] != [7, 8, 9]:
        raise SystemExit(f"pong ids {[m[1] for m in pongs]}")
    for m in pongs:
        if m[2] != 0x11110000 + m[1] or m[3] != 0x22 or m[5] != 0:
            raise SystemExit(f"pong echo {m}")
    ack = msgs[4]
    if ack[1:4] != (1, 1, 0):
        raise SystemExit(f"flood ACK {ack}")
    data = [m for m in msgs if m[0] == 2]
    if [m[1] for m in data] != list(range(1, count + 1)):
        raise SystemExit("data sequence is not 1..%d" % count)
    last_counter = 0
    for m in data:
        want = tuple(ring_mix(m[1] * 4 + i) for i in range(4))
        if m[4:8] != want:
            raise SystemExit(f"pattern of message {m[1]}: {m[4:8]}")
        if m[3] < last_counter or m[3] < m[1]:
            raise SystemExit(f"counter {m[3]} at message {m[1]}")
        last_counter = m[3]
    done = msgs[-1]
    if done[0] != 3 or done[1] != count or done[3] < last_counter or done[5] != 3:
        raise SystemExit(f"DONE message {done}")
    if len(msgs) != 1 + 3 + 1 + count + 1:
        raise SystemExit(f"{len(msgs)} messages recorded")

    # Run B: flood, ten pings, back to quiet, one more ping, then stop. The
    # app sends no DATA after the ACK for quiet and still answers.
    script = [ring_ctrl(1, 1)] + [ring_ping(20 + i) for i in range(10)]
    script += [ring_ctrl(2, 0)] + [ring_ping(97 + i) for i in range(3)]
    msgs = ring_run(exe, script, [], stop=True)
    kinds = [m[0] for m in msgs]
    if kinds[:2] != [1, 8] or kinds[-1] != 3 or 2 in kinds:
        raise SystemExit(f"quiet switch sent {kinds}")
    acks = [m for m in msgs if m[0] == 8]
    if [a[1:4] for a in acks] != [(1, 1, 0), (2, 0, 0)]:
        raise SystemExit(f"ACKs {acks}")
    after = msgs[msgs.index(acks[1]) + 1:-1]
    if not after or any(m[0] != 6 for m in after):
        raise SystemExit(f"after the quiet ACK the app sent {after}")
    done = msgs[-1]
    if done[1] != 0 or done[5] != len([m for m in msgs if m[0] == 6]):
        raise SystemExit(f"DONE {done}")


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

def check_pixelsstop(exe):
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
        time.sleep(0.5)  # let pixels block in zz_display_flip
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise SystemExit("display flip not interrupted by SIGTERM")
        if rc != 0:
            raise SystemExit(f"expected clean exit 0, got {rc}")
    finally:
        proc.stdin.close()
        proc.kill()
        proc.wait()


def check_msgwaitidle(exe):
    if os.name != "posix":
        return
    import resource
    import time

    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    proc = run(exe, ["--headless", "--", "msgwaitidle"])
    wall = time.monotonic() - start
    expect_ok(proc)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)
    if wall < 1.4:
        raise SystemExit(f"idle wait ended after {wall:.2f} s")
    if cpu > 0.3 * wall:
        raise SystemExit(f"idle wait burned {cpu:.2f} s CPU in {wall:.2f} s")


def check_msgwaitstop(exe):
    if os.name != "posix":
        return
    import signal
    import time

    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    proc = subprocess.Popen(
        [exe, "--headless", "--", "msgwaitstop"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
    )
    try:
        time.sleep(0.5)  # let the app park in zz_msg_wait with 30 s to go
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise SystemExit("zz_msg_wait not interrupted by SIGTERM")
        if rc != 0:
            raise SystemExit(f"expected ZZ_EIO and exit 0, got {rc}")
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
        proc = run(exe, ["--headless", "--", "msgwait"])
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
    "ringflood": check_ringflood,
    "freestanding": check_freestanding,
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
    "pixelsstop": check_pixelsstop,
    "msgwaitstop": check_msgwaitstop,
    "msgwaitidle": check_msgwaitidle,
}


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in CHECKS:
        raise SystemExit("usage: check_runner.py <test> <exe>")
    CHECKS[sys.argv[1]](sys.argv[2])


if __name__ == "__main__":
    main()
