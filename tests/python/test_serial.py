#!/usr/bin/env python3
import os
import pty
import subprocess
import sys
import threading
import time


def pump(src_fd, dst_fd, stop_event):
    try:
        while not stop_event.is_set():
            try:
                data = os.read(src_fd, 1024)
            except OSError:
                break
            if not data:
                # EOF
                break
            try:
                os.write(dst_fd, data)
            except OSError:
                break
    finally:
        try:
            os.close(src_fd)
        except Exception:
            pass
        try:
            os.close(dst_fd)
        except Exception:
            pass


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: test_serial.py <path-to-test-binary>")
        sys.exit(2)

    test_binary = sys.argv[1]

    # Create two pty pairs (master, slave)
    m1, s1 = pty.openpty()
    m2, s2 = pty.openpty()

    slave1_name = os.ttyname(s1)
    slave2_name = os.ttyname(s2)

    print(f"Virtual linked ports: {slave1_name} <-> {slave2_name}")
    sys.stdout.flush()

    stop_event = threading.Event()

    # Start relay threads: master1 -> master2 and master2 -> master1
    t1 = threading.Thread(target=pump, args=(m1, m2, stop_event), daemon=True)
    t2 = threading.Thread(target=pump, args=(m2, m1, stop_event), daemon=True)
    t1.start()
    t2.start()

    # Give the relay a moment to start
    time.sleep(0.05)

    # Run the test binary with the two slave device paths
    proc = subprocess.Popen([test_binary, slave1_name, slave2_name] + sys.argv[2:], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    try:
        stdout, stderr = proc.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
        print("Test binary timed out", file=sys.stderr)
        stop_event.set()
        # still print whatever we got
    # Stop relay
    stop_event.set()
    time.sleep(0.05)

    # Always print child's outputs and return code for diagnostics
    print("=== CHILD STDOUT ===")
    if stdout:
        print(stdout)
    else:
        print("<no stdout>")
    print("=== CHILD STDERR ===", file=sys.stderr)
    if stderr:
        print(stderr, file=sys.stderr)
    else:
        print("<no stderr>", file=sys.stderr)

    print(f"Child exit code: {proc.returncode}")
    sys.stdout.flush()
    sys.stderr.flush()

    # write small log for CI inspection
    try:
        with open('/tmp/hyserial_test_runner.log', 'a') as f:
            f.write(f"Binary: {test_binary} args: {sys.argv[2:]}\n")
            f.write(f"Slave ports: {slave1_name} {slave2_name}\n")
            f.write("STDOUT:\n")
            f.write(stdout or "<no stdout>\n")
            f.write("STDERR:\n")
            f.write((stderr or "<no stderr>\n") + "\n")
            f.write(f"Exit code: {proc.returncode}\n---\n")
    except Exception:
        pass

    sys.exit(proc.returncode if proc.returncode is not None else 1)
