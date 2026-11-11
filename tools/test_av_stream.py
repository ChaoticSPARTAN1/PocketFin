# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
import struct
import unittest

from av_stream import (Packet, Reader, Receiver, encode, configuration, HEADER, LIMIT,
    CONFIG, VIDEO, AUDIO, END, KEYFRAME, DISCONTINUITY, StreamError)
from jpeg_payload import compact


class StreamTests(unittest.TestCase):
    def test_fragmented_packet_checksum_limits_and_disconnect(self):
        packet = Packet(VIDEO, 91, 1, 90000, b'compressed-frame', KEYFRAME)
        raw = encode(packet)
        reader = Reader(); result = []
        for byte in raw:
            result += reader.feed(bytes([byte]))
        reader.finish()
        self.assertEqual(result, [packet])
        for bad in (b'BAD!'+raw[4:], raw[:-1]+bytes([raw[-1]^1])):
            with self.assertRaises(StreamError):
                Reader().feed(bad)
        bad = bytearray(raw[:32]);struct.pack_into('<I',bad,24,LIMIT+1)
        with self.assertRaises(StreamError):
            Reader().feed(bad)
        reader.feed(raw[:-1])
        with self.assertRaises(StreamError):
            reader.finish()

    def test_loss_requires_video_keyframe_and_audio_is_independent(self):
        rx = Receiver()
        rx.accept(Packet(CONFIG, 51, 0, 0, configuration()))
        self.assertIsNone(rx.accept(Packet(VIDEO, 51, 1, 0, b'predicted')))
        key = Packet(VIDEO, 51, 2, 0, b'independent', KEYFRAME)
        self.assertEqual(rx.accept(key),key)
        self.assertIsNone(rx.accept(Packet(VIDEO, 51, 4, 3000, b'gap')))
        self.assertEqual(rx.lost_packets,1)
        audio = Packet(AUDIO,51,5,3000,b'audio-block')
        self.assertEqual(rx.accept(audio),audio)
        key = Packet(VIDEO,51,6,6000,b'keyframe',KEYFRAME)
        self.assertEqual(rx.accept(key),key)
        self.assertIsNone(rx.accept(key))  # duplicate
        rx.accept(Packet(END,51,7,9000))
        self.assertTrue(rx.closed)
        self.assertIsNone(rx.accept(Packet(AUDIO,51,8,9000,b'late')))

    def test_new_session_and_discontinuity_reset_the_timeline(self):
        rx = Receiver();rx.accept(Packet(CONFIG,13,0,0,configuration()))
        rx.accept(Packet(VIDEO,13,1,90000,b'key',KEYFRAME))
        with self.assertRaises(StreamError):
            rx.accept(Packet(VIDEO,13,2,0,b'bad',KEYFRAME))
        p=Packet(VIDEO,13,3,0,b'seek',KEYFRAME|DISCONTINUITY)
        self.assertEqual(rx.accept(p),p)
        self.assertIsNone(rx.accept(Packet(VIDEO,14,4,3000,b'foreign',KEYFRAME)))
        rx.accept(Packet(CONFIG,14,0,0,configuration()))
        self.assertEqual(rx.session,14)

    def test_unsupported_config_and_jpeg_are_rejected(self):
        for args in ({'fps':15},{'width':640},{'rate':8000}):
            with self.assertRaises(StreamError):configuration(**args)
        for jpeg in (b'',b'\xff\xd8\xff\xc4\0\x40',b'\xff\xd8\xff\xc2\0\x02'):
            with self.assertRaises(ValueError):compact(jpeg)


if __name__ == '__main__':
    unittest.main()
