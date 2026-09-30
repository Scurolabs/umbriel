import ctypes
import os
import socket
import struct
import sys
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


if __name__ == '__main__':
    BINARY = sys.argv.pop(1)
    unittest.main()
