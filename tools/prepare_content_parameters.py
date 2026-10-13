"""Extract validated TLG user parameters; no SKU or license state is inferred."""
import argparse
from pathlib import Path
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from prepare import sfo


def encode(values):
    if values.get('TITLE_ID')!='CUSA03627' or values.get('APP_VER')!='01.03':
        raise ValueError('Expected CUSA03627 01.03 metadata')
    params=[values[f'USER_DEFINED_PARAM_{i}'] for i in range(1,5)]
    if any(type(v) is not int or not 0<=v<=0xffffffff for v in params):
        raise ValueError('Invalid user-defined parameter')
    return struct.pack('<8s16s8s4I',b'TLGPAR01',b'CUSA03627',b'01.03',*params)


def encode_gpu(values):
    encode(values)
    attribute=values.get('ATTRIBUTE')
    if type(attribute) is not int or not 0<=attribute<=0xffffffff:
        raise ValueError('Invalid SFO ATTRIBUTE')
    return struct.pack('<8s16s8sI',b'TLGGPU01',b'CUSA03627',b'01.03',attribute)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game',type=Path)
    parser.add_argument('--out',type=Path,default=Path('out/tlg/content-parameters.bin'))
    args=parser.parse_args()
    values=sfo((args.game/'sce_sys/param.sfo').read_bytes())
    data=encode(values)
    gpu=encode_gpu(values)
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_bytes(data)
    args.out.with_name('gpu-parameters.bin').write_bytes(gpu)
