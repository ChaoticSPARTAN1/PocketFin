# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Worker lifecycle, bounded backpressure, RPC retries and secret isolation."""
import io
import json
import struct
import subprocess
import sys
import threading
import time
import unittest

from av_stream import Packet, encode, configuration, CONFIG, VIDEO, AUDIO, END, KEYFRAME
from link_gateway import (Gateway, HELLO, HOME, STREAM_START, STREAM_READ, STREAM_STOP,
                          encode_packet, read_packet, RESPONSE, ERROR)
from stream_worker import (StreamManager, StreamWorker, READ_HEADER, DATA, WAIT, EOF,
                           FAILED, QUEUE_PACKETS)
from test_link_gateway import FakeClient


def raw_packets(session=3, count=1):
    yield encode(Packet(CONFIG, session, 0, 0, configuration(block=256)))
    for index in range(count):
        yield encode(Packet(VIDEO, session, index+1, index*3000, bytes(12000), KEYFRAME))
    yield encode(Packet(END, session, count+1, count*3000))


def wait_for(predicate):
    deadline = time.monotonic() + 3
    while not predicate():
        if time.monotonic() > deadline:
            raise AssertionError('Bounded worker test timed out')
        time.sleep(.01)


class WorkerTests(unittest.TestCase):
    def test_backpressure_is_bounded_and_close_releases_producer(self):
        released = threading.Event()
        def source(stop, process):
            try:
                yield from raw_packets(count=10000)
            finally:
                released.set()
        worker = StreamWorker(3, source)
        try:
            wait_for(worker.queue.full)
            self.assertEqual(worker.queue.qsize(), QUEUE_PACKETS)
            time.sleep(.03)
            self.assertFalse(worker.done.is_set())
        finally:
            worker.close()
        self.assertTrue(released.is_set())
        self.assertFalse(worker.thread.is_alive())

    def test_read_retry_exactness_and_64bit_offsets(self):
        worker = StreamWorker(3, lambda stop, process: raw_packets(count=2))
        original = b''.join(raw_packets(count=2))
        try:
            wait_for(worker.done.is_set)
            data, offset = bytearray(), 0
            while True:
                response = worker.read(offset)
                session, actual, status, reserved, length = READ_HEADER.unpack_from(response)
                self.assertEqual((session, actual, reserved), (3, offset, 0))
                self.assertEqual(length, len(response)-READ_HEADER.size)
                if status == EOF:
                    break
                self.assertEqual(status, DATA)
                self.assertEqual(worker.read(offset), response)
                data.extend(response[READ_HEADER.size:]); offset += length
            self.assertEqual(data, original)
            with self.assertRaises(ValueError):
                worker.read(1)
            # Offset field must not wrap at 4 GiB during a long title.
            worker.last_reply = None
            worker.offset = 2**32 + 8
            self.assertEqual(READ_HEADER.unpack(worker.read(2**32+8))[1], 2**32+8)
        finally:
            worker.close()

    def test_idle_or_stalled_encoder_cancels_without_a_further_rpc(self):
        for idle, stall in ((.12, 5), (5, .12)):
            def source(stop, process):
                stop.wait(2)
                if False:
                    yield b''
            worker = StreamWorker(3, source, idle_seconds=idle, stall_seconds=stall)
            try:
                wait_for(worker.done.is_set)
                self.assertTrue(worker.cancelled.is_set())
                self.assertEqual(READ_HEADER.unpack(worker.read(0))[2], FAILED)
            finally:
                worker.close()

    def test_truncated_session_is_failure_and_not_clean_eof(self):
        def source(stop, process):
            for packet in list(raw_packets())[:-1]:
                yield packet
        worker = StreamWorker(3, source)
        try:
            wait_for(worker.done.is_set)
            self.assertEqual(READ_HEADER.unpack(worker.read(0))[2], FAILED)
        finally:
            worker.close()

    def test_close_terminates_owned_encoder_even_when_read_is_blocked(self):
        registered = threading.Event()
        child = []
        def source(stop, process):
            proc = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            child.append(proc); process(proc); registered.set()
            try:
                proc.wait(timeout=10)
                if False:
                    yield b''
            finally:
                if proc.poll() is None:
                    proc.kill(); proc.wait(timeout=2)
                process(None)
        worker = StreamWorker(3, source)
        self.assertTrue(registered.wait(2))
        worker.close()
        self.assertIsNotNone(child[0].poll())
        self.assertFalse(worker.thread.is_alive())

    def test_producer_cannot_switch_sessions_or_skip_sequences(self):
        for raw in (list(raw_packets(4)), [encode(Packet(CONFIG,3,0,0,configuration(block=256))),
                    encode(Packet(VIDEO,3,2,0,b'video',KEYFRAME))]):
            worker = StreamWorker(3, lambda stop,process: iter(raw))
            try:
                wait_for(worker.done.is_set)
                self.assertEqual(READ_HEADER.unpack(worker.read(0))[2], FAILED)
            finally:
                worker.close()

    def test_gateway_starts_by_handle_and_reuses_mutating_rpc_reply(self):
        called = []
        def factory(profile, session, seek, duration):
            called.append((profile.fps, seek, duration))
            return lambda stop, process: raw_packets(session)
        streams = StreamManager(FakeClient(), factory)
        gateway = Gateway(FakeClient(), streams)
        handle = gateway.dispatch(HOME, {})['rows'][0]['items'][0]['handle']
        raw = json.dumps({'handle': handle, 'start_seconds': 2}).encode()
        request = encode_packet(STREAM_START, 17, raw)
        changed = encode_packet(STREAM_START, 17, json.dumps({'handle': handle}).encode())
        output = io.BytesIO()
        gateway.serve(io.BytesIO(request+request+changed), output)
        replies = io.BytesIO(output.getvalue())
        first, second, third = (read_packet(replies) for _ in range(3))
        self.assertEqual(first, second)
        self.assertEqual(third[1], RESPONSE|ERROR)
        self.assertEqual(called, [(30, 2, 8)])
        for forbidden in (b'PRIVATE_PATH', b'PRIVATE_TOKEN', b'server-video'):
            self.assertNotIn(forbidden, output.getvalue())
        self.assertIsNone(streams.worker)  # EOF on RPC pipe cancels child.

    def test_manager_validates_profile_seek_session_and_explicit_stop(self):
        manager = StreamManager(FakeClient(), lambda p,s,k,d: lambda stop,process: raw_packets(s))
        try:
            for settings in ({'profile':'pocket-link-h264-30'}, {'start_seconds':True},
                             {'start_seconds':float('nan')}, {'start_seconds':11}):
                with self.assertRaises(ValueError):
                    manager.start(FakeClient.video, settings)
            result = manager.start(FakeClient.video, {})
            with self.assertRaises(ValueError):
                manager.start(FakeClient.video, {})
            with self.assertRaises(ValueError):
                manager.read({'session': result['session']+1})
            self.assertEqual(manager.stop(result), {'stopped':True})
            self.assertIsNone(manager.worker)
        finally:
            manager.close()


if __name__ == '__main__':
    unittest.main()
