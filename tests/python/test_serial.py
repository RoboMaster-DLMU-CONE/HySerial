#!/usr/bin/env python3
import os
import pty
import subprocess
import sys
import time
import selectors
import fcntl
import errno
import re


def set_nonblocking(fd):
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)


def safe_read(fd, max_bytes=4096):
    try:
        return os.read(fd, max_bytes)
    except InterruptedError:
        return b""
    except BlockingIOError:
        return b""
    except OSError:
        return b""


def safe_write_all(fd, data):
    # write all bytes handling partial writes and EAGAIN/EINTR
    total = 0
    view = memoryview(data)
    while total < len(data):
        try:
            n = os.write(fd, view[total:])
            if n == 0:
                raise BrokenPipeError
            total += n
        except InterruptedError:
            continue
        except BlockingIOError:
            # caller should wait for fd to be writable; sleep briefly
            time.sleep(0.001)
            continue
        except BrokenPipeError:
            return total
        except OSError:
            return total
    return total


def relay_loop(master_a, master_b, proc, timeout_sec=30.0):
    sel = selectors.DefaultSelector()
    sel.register(master_a, selectors.EVENT_READ, data=('A', master_b))
    sel.register(master_b, selectors.EVENT_READ, data=('B', master_a))

    deadline = time.time() + timeout_sec
    alive = True
    try:
        while True:
            # check child process
            if proc.poll() is not None:
                break
            now = time.time()
            if now > deadline:
                # timeout
                try:
                    proc.kill()
                except Exception:
                    pass
                break
            timeout = max(0, min(0.1, deadline - now))
            events = sel.select(timeout)
            for key, mask in events:
                src_fd = key.fileobj
                _, dst_fd = key.data
                data = safe_read(src_fd, 4096)
                if not data:
                    # EOF or no data; if EOF, close registration for this fd
                    # We don't close the destination immediately because other side may still send
                    continue
                # write out, handling partial writes
                written = 0
                while written < len(data):
                    try:
                        n = os.write(dst_fd, data[written:])
                        if n == 0:
                            raise BrokenPipeError
                        written += n
                    except InterruptedError:
                        continue
                    except BlockingIOError:
                        # wait a bit before retrying
                        time.sleep(0.001)
                        continue
                    except BrokenPipeError:
                        # destination closed; stop writing
                        break
                    except OSError:
                        # other error, stop
                        break
            # small sleep to avoid busy loop when no events
            if not events:
                time.sleep(0.001)
    finally:
        try:
            sel.unregister(master_a)
        except Exception:
            pass
        try:
            sel.unregister(master_b)
        except Exception:
            pass


def create_socat_pair(timeout=5.0):
    """
    Try to start socat to create a connected PTY pair. Returns (proc, slave1, slave2) on success, or None.
    """
    try:
        p = subprocess.Popen(['socat', '-d', '-d', 'pty,raw,echo=0', 'pty,raw,echo=0'], stderr=subprocess.PIPE,
                             stdout=subprocess.DEVNULL, text=True)
    except FileNotFoundError:
        return None

    slaves = []
    start = time.time()
    # Read stderr lines until we have two PTY paths or timeout
    try:
        while time.time() - start < timeout and len(slaves) < 2:
            line = p.stderr.readline()
            if not line:
                time.sleep(0.01)
                continue
            m = re.search(r'(/dev/pts/\d+)', line)
            if m:
                path = m.group(1)
                if path not in slaves:
                    slaves.append(path)
            # socat may also print PTY names in different formats, try alternate pattern
            m2 = re.search(r'PTY is (.+)', line)
            if m2:
                path = m2.group(1).strip()
                if path.startswith('/dev') and path not in slaves:
                    slaves.append(path)
    except Exception:
        pass

    if len(slaves) < 2:
        try:
            p.kill()
        except Exception:
            pass
        try:
            p.wait(timeout=1)
        except Exception:
            pass
        return None

    return (p, slaves[0], slaves[1])


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: test_serial.py <path-to-test-binary>")
        sys.exit(2)

    test_binary = sys.argv[1]

    # Try socat first for a robust PTY pair
    socat_res = create_socat_pair(timeout=5.0)

    if socat_res is not None:
        socat_proc, slave1_name, slave2_name = socat_res
        print(f"Virtual linked ports (socat): {slave1_name} <-> {slave2_name}")
        sys.stdout.flush()

        proc = subprocess.Popen([test_binary, slave1_name, slave2_name] + sys.argv[2:], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        try:
            # wait for process while socat runs in background linking the PTYs
            stdout, stderr = proc.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            try:
                proc.kill()
            except Exception:
                pass
            stdout, stderr = proc.communicate()
        finally:
            # cleanup socat
            try:
                socat_proc.kill()
            except Exception:
                pass
            try:
                socat_proc.wait(timeout=1)
            except Exception:
                pass

    else:
        # Fallback to internal PTY pair + relay
        m1, s1 = pty.openpty()
        m2, s2 = pty.openpty()

        # set masters non-blocking
        set_nonblocking(m1)
        set_nonblocking(m2)

        slave1_name = os.ttyname(s1)
        slave2_name = os.ttyname(s2)

        print(f"Virtual linked ports: {slave1_name} <-> {slave2_name}")
        sys.stdout.flush()

        proc = subprocess.Popen([test_binary, slave1_name, slave2_name] + sys.argv[2:], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)

        try:
            relay_loop(m1, m2, proc, timeout_sec=30.0)
            # after relay loop completes, wait a short time for the child to exit and collect output
            try:
                stdout, stderr = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()
        except Exception as e:
            try:
                proc.kill()
            except Exception:
                pass
            stdout, stderr = proc.communicate()
            print(f"Relay error: {e}", file=sys.stderr)

        # Close master/slave fds
        try:
            os.close(m1)
        except Exception:
            pass
        try:
            os.close(m2)
        except Exception:
            pass
        try:
            os.close(s1)
        except Exception:
            pass
        try:
            os.close(s2)
        except Exception:
            pass

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
