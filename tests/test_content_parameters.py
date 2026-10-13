import importlib.util
from pathlib import Path
import struct
import unittest
spec=importlib.util.spec_from_file_location('content_parameters',Path(__file__).resolve().parents[1]/'tools/prepare_content_parameters.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ContentParameterTests(unittest.TestCase):
    def values(self):
        return dict(TITLE_ID='CUSA03627',APP_VER='01.03',**{f'USER_DEFINED_PARAM_{i}':i for i in range(1,5)})

    def test_exact_layout_preserves_values(self):
        data=module.encode(self.values())
        self.assertEqual(len(data),48)
        self.assertEqual(struct.unpack('<8s16s8s4I',data)[-4:],(1,2,3,4))

    def test_rejects_wrong_identity_missing_or_invalid_parameter(self):
        for key,value in [('TITLE_ID','other'),('APP_VER','01.00'),('USER_DEFINED_PARAM_1',-1),('USER_DEFINED_PARAM_2',True)]:
            values=self.values();values[key]=value
            with self.assertRaises(ValueError): module.encode(values)
        values=self.values();del values['USER_DEFINED_PARAM_4']
        with self.assertRaises(KeyError): module.encode(values)
