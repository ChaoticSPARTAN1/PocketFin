# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Real FFmpeg/PyAV worker integration with synthetic local fixtures only."""
import io
from pathlib import Path
import re
import tempfile
import time
import unittest

from av_stream import Reader, Receiver, CONFIG, VIDEO, AUDIO, END, CONFIG_PAYLOAD
from connection_config import ROOT
from stream_transport_probe import quiet_run
from stream_worker import StreamManager, READ_HEADER, DATA, WAIT, EOF


class LiveAVTests(unittest.TestCase):
    def test_silent_video_gets_audio_clock_and_natural_end_reaps_encoder(self):
        private = ROOT/'build/test-fixtures';private.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=private) as path:
            video = Path(path)/'silent.avi'
            quiet_run(['ffmpeg','-nostdin','-v','error','-y','-f','lavfi','-i',
                'testsrc2=size=160x144:rate=30','-t','1.2','-an','-c:v','mjpeg',str(video)],15)
            content = video.read_bytes()
            class Client:
                def open_video(self, identifier, byte_range=None):
                    class Response(io.BytesIO):
                        pass
                    start,end = 0,len(content)-1
                    if byte_range:
                        match = re.fullmatch(r'bytes=(\d+)-(\d*)',byte_range)
                        if not match:
                            raise ValueError('Invalid synthetic range')
                        start=int(match[1]);end=min(end,int(match[2])) if match[2] else end
                    data=content[start:end+1]
                    response=Response(data)
                    response.status=206 if byte_range else 200
                    response.headers={'Content-Type':'video/x-msvideo','Content-Length':str(len(data)),
                        'Accept-Ranges':'bytes','Content-Range':f'bytes {start}-{end}/{len(content)}'}
                    return response
            manager=StreamManager(Client())
            try:
                session=manager.start({'Id':'synthetic','Type':'Video','RunTimeTicks':12000000},{})['session']
                worker=manager.worker
                reader,receiver=Reader(),Receiver()
                offset=0;counts={CONFIG:0,VIDEO:0,AUDIO:0,END:0};ending=None
                deadline=time.monotonic()+15
                while True:
                    self.assertLess(time.monotonic(),deadline)
                    reply=manager.read({'session':session,'offset':offset})
                    _,actual,status,_,size=READ_HEADER.unpack_from(reply)
                    self.assertEqual(actual,offset)
                    if status==WAIT:
                        time.sleep(.005);continue
                    if status==EOF:
                        break
                    self.assertEqual(status,DATA)
                    for packet in reader.feed(reply[READ_HEADER.size:]):
                        self.assertEqual(receiver.accept(packet),packet)
                        counts[packet.kind]+=1
                        if packet.kind==CONFIG:
                            self.assertEqual(CONFIG_PAYLOAD.unpack(packet.payload)[4],16000)
                        if packet.kind==END:
                            ending=packet.pts
                    offset+=size
                reader.finish()
                self.assertTrue(receiver.closed)
                self.assertEqual(counts[VIDEO],36)
                self.assertGreater(counts[AUDIO],30)
                self.assertEqual(ending,108000)
                self.assertEqual(counts[END],1)
                self.assertTrue(worker.done.is_set())
                self.assertIsNone(worker.proc)
            finally:
                manager.close()


if __name__=='__main__':
    unittest.main()
