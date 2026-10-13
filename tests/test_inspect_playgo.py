import importlib.util
from pathlib import Path
import struct
import unittest
spec=importlib.util.spec_from_file_location('inspect_playgo',Path(__file__).resolve().parents[1]/'tools/inspect_playgo.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PlayGoInspectionTests(unittest.TestCase):
    def image(self):
        data=bytearray(312)
        data[:4]=b'plgo'
        struct.pack_into('<4HI',data,8,1,1,1,0,len(data))
        for i,(offset,length) in enumerate([(256,32),(288,2),(290,6),(296,16)]):
            struct.pack_into('<II',data,192+8*i,offset,length)
        struct.pack_into('<H',data,270,1)
        data[290:296]=b'chunk\0'
        struct.pack_into('<QQ',data,296,1234,5678)
        return data

    def test_reports_package_ranges_without_claiming_availability(self):
        result=module.inspect(self.image())
        self.assertEqual(result['chunks'],1)
        self.assertEqual(result['chunk_references'][0]['microchunks'],[0])
        self.assertEqual(result['package_ranges'][0]['package_offset'],1234)
        self.assertIn('unverified',result['availability'])

    def test_rejects_truncation_and_out_of_bounds_tables(self):
        data=self.image();struct.pack_into('<I',data,192,9999)
        for bad in (b'',self.image()[:-1],data):
            with self.assertRaises(ValueError): module.inspect(bad)

    def test_rejects_invalid_microchunk_and_label(self):
        data=self.image();struct.pack_into('<H',data,288,1)
        with self.assertRaises(ValueError): module.inspect(data)
        data=self.image();data[295]=65
        with self.assertRaises(ValueError): module.inspect(data)
