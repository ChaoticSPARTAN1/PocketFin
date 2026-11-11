# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""In-memory transport tests; no physical console or saved credentials."""
import io
import json
import struct
import unittest
from unittest.mock import patch

from link_gateway import (Gateway, encode_packet, read_packet, ProtocolError, MAX_PAYLOAD,
                          HELLO, HOME, BROWSE, DETAILS, ART, MEDIA, RESPONSE, ERROR)


class FakeClient:
    video = {"Id": "server-video", "Name": "Synthetic Movie", "Type": "Movie", "RunTimeTicks": 100000000,
             "UserData": {"PlaybackPositionTicks": 50000000, "Played": False},
             "MediaSources": [{"Path": "PRIVATE_PATH"}], "AccessToken": "PRIVATE_TOKEN"}

    def home(self):
        return [{"title": "Latest Movies", "items": [self.video]}]

    def browse(self, parent, search, start):
        return {"Items": [self.video], "TotalRecordCount": 1}

    def item(self, identifier):
        return dict(self.video, Overview="Synthetic overview", Genres=["Adventure"])

    def open_video(self, identifier, byte_range):
        class Response(io.BytesIO):
            status = 206
            headers = {"Content-Range": "bytes 0-3/4"}
        return Response(b"1234")


class GatewayTests(unittest.TestCase):
    def test_fragmented_transport_crc_and_lengths(self):
        packet = encode_packet(HOME, 31, b"{}")

        class Fragmented(io.BytesIO):
            def read(self, size=-1):
                return super().read(min(size, 3))

        self.assertEqual(read_packet(Fragmented(packet)), (HOME, 0, 31, b"{}"))
        for broken in (packet[:-1], packet[:-1] + b"X", b"BAD!" + packet[4:]):
            with self.assertRaises(ProtocolError):
                read_packet(io.BytesIO(broken))
        with self.assertRaises(ProtocolError):
            encode_packet(HOME, 1, bytes(MAX_PAYLOAD + 1))

    def test_real_api_items_become_opaque_handles_with_resume_state(self):
        gateway = Gateway(FakeClient())
        rows = gateway.dispatch(HOME, {})["rows"]
        public = rows[0]["items"][0]
        self.assertEqual(public["resume_ticks"], 50000000)
        self.assertNotIn("Id", public)
        detail = gateway.dispatch(DETAILS, {"handle": public["handle"]})
        self.assertEqual(detail["overview"], "Synthetic overview")
        encoded = json.dumps(detail)
        for private in ("PRIVATE_PATH", "PRIVATE_TOKEN", "server-video"):
            self.assertNotIn(private, encoded)
        self.assertFalse(gateway.dispatch(HELLO, {})["hardware_connected"])
        self.assertFalse(gateway.dispatch(HELLO, {})["pocket_decoder_ready"])

    def test_chunked_artwork_original_media_and_bad_handle(self):
        gateway = Gateway(FakeClient())
        handle = gateway.dispatch(BROWSE, {})["items"][0]["handle"]
        raw = bytes((x % 256 for x in range(64 * 96 * 2)))
        with patch("link_gateway.fetch_card", return_value=raw):
            chunks = [gateway.dispatch(ART, {"handle": handle, "offset": offset}) for offset in (0, 4096, 8192)]
        self.assertEqual(struct.unpack("<IHH", chunks[0][:8]), (64 * 96 * 2, 64, 96))
        self.assertEqual(b"".join(chunk[8:] for chunk in chunks), raw)
        self.assertEqual(gateway.dispatch(MEDIA, {"handle": handle}), b"1234")
        with self.assertRaises(ProtocolError):
            gateway.dispatch(DETAILS, {"handle": 9999})
        with self.assertRaises(ProtocolError):
            gateway.dispatch(MEDIA, {"handle": handle, "offset": 20})

    def test_correlated_errors_do_not_reflect_upstream_secrets(self):
        gateway = Gateway(FakeClient())
        request = encode_packet(DETAILS, 71, b'{"handle":9,"password":"DO_NOT_REFLECT"}')
        output = io.BytesIO()
        gateway.serve(io.BytesIO(request), output)
        command, flags, sequence, payload = read_packet(io.BytesIO(output.getvalue()))
        self.assertEqual((command, flags, sequence), (DETAILS, RESPONSE | ERROR, 71))
        self.assertNotIn(b"DO_NOT_REFLECT", payload)

    def test_large_unicode_home_is_bounded(self):
        client = FakeClient()
        client.home = lambda: [{"title": "\U0001f680" * 160,
            "items": [dict(FakeClient.video, Id=f"v{row}-{i}", Name="\U0001f680" * 160) for i in range(12)]}
            for row in range(12)]
        result = Gateway(client).dispatch(HOME, {})
        self.assertLessEqual(len(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode()), MAX_PAYLOAD)
        self.assertTrue(result["truncated"])


if __name__ == "__main__":
    unittest.main()
