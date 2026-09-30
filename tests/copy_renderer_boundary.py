import ctypes
import os
import signal
import socket
import struct
import sys
import time
import unittest


class Attributes(ctypes.Structure):
    _fields_ = [('width', ctypes.c_int32), ('height', ctypes.c_int32),
                ('format', ctypes.c_uint32), ('modifier', ctypes.c_uint64),
                ('planes', ctypes.c_int), ('offsets', ctypes.c_uint32 * 4),
                ('strides', ctypes.c_uint32 * 4), ('fds', ctypes.c_int * 4)]


class Request(ctypes.Structure):
    _fields_ = [('operation', ctypes.c_uint32), ('source', Attributes), ('destination', Attributes)]


class WorkerBoundaryTests(unittest.TestCase):
    def run_worker(self, data, descriptors=()):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        actions = [(os.POSIX_SPAWN_DUP2, child.fileno(), 3)]
        pid = os.posix_spawn(BINARY, [BINARY, '--internal-copy-renderer'], os.environ, file_actions=actions)
        child.close()
        try:
            control = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack(f'{len(descriptors)}i', *descriptors))] \
                if descriptors else []
            parent.sendmsg([data], control)
            _, status = os.waitpid(pid, 0)
            self.assertEqual(os.waitstatus_to_exitcode(status), 1)
        finally:
            parent.close()

    def test_truncated_initialization_is_rejected(self):
        self.run_worker(b'\x01')

    def test_initialization_requires_one_drm_descriptor(self):
        self.run_worker(bytes(Request(operation=1)))
        fd = os.open('/dev/null', os.O_RDWR | os.O_CLOEXEC)
        try:
            self.run_worker(bytes(Request(operation=1)), (fd,))
            self.run_worker(bytes(Request(operation=1)), (fd, fd))
        finally:
            os.close(fd)

    def test_oversized_ancillary_descriptor_list_is_rejected(self):
        fd = os.open('/dev/null', os.O_RDWR | os.O_CLOEXEC)
        try:
            self.run_worker(bytes(Request(operation=1)), (fd,) * 9)
        finally:
            os.close(fd)

    def test_unknown_initial_operation_is_rejected(self):
        self.run_worker(bytes(Request(operation=99)))

    def test_worker_unblocks_inherited_termination_signal(self):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        actions = [(os.POSIX_SPAWN_DUP2, child.fileno(), 3)]
        pid = os.posix_spawn(BINARY, [BINARY, '--internal-copy-renderer'], os.environ,
                             file_actions=actions, setsigmask=(signal.SIGTERM,))
        child.close()
        waited = 0
        try:
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                with open(f'/proc/{pid}/status') as status:
                    blocked = next(line for line in status if line.startswith('SigBlk:'))
                if not int(blocked.split()[1], 16) & (1 << (signal.SIGTERM - 1)):
                    break
                time.sleep(0.005)
            else:
                self.fail('Worker kept the compositor termination mask')
            os.kill(pid, signal.SIGTERM)
            while time.monotonic() < deadline:
                waited, status = os.waitpid(pid, os.WNOHANG)
                if waited:
                    self.assertEqual(os.waitstatus_to_exitcode(status), -signal.SIGTERM)
                    break
                time.sleep(0.005)
            else:
                self.fail('Worker did not accept the termination signal')
        finally:
            parent.close()
            if not waited:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)


if __name__ == '__main__':
    BINARY = sys.argv.pop(1)
    unittest.main()
