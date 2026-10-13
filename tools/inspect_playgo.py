"""Inspect PlayGo tables without inferring extracted-file installation status.
Format reference: shadPS4 core/file_format/playgo_chunk.h (GPL-2.0-or-later,
Copyright 2024 shadPS4 Emulator Project).
"""
import argparse
import json
from pathlib import Path
import struct


def inspect(data):
    if len(data)<256 or data[:4]!=b'plgo':
        raise ValueError('Invalid PlayGo header')
    images,chunks,mchunks,scenarios=struct.unpack_from('<4H',data,8)
    if struct.unpack_from('<I',data,16)[0]!=len(data) or not 0<chunks<=1000 or mchunks>8000 or scenarios>32 or images>1:
        raise ValueError('Invalid counts or file size')
    tables=[]
    for i in range(8):
        offset,length=struct.unpack_from('<II',data,192+8*i)
        if length and (offset<256 or offset>len(data) or length>len(data)-offset):
            raise ValueError('Table outside file')
        tables.append(data[offset:offset+length])
    attrs,refs,labels,physical,scenario_attrs,scenario_refs,scenario_labels,inner=tables
    if len(attrs)!=chunks*32 or len(physical)!=mchunks*16 or len(scenario_attrs)!=scenarios*32:
        raise ValueError('Wrong attribute table size')
    result=[]
    for i in range(chunks):
        count=struct.unpack_from('<H',attrs,i*32+14)[0]
        offset,label=struct.unpack_from('<II',attrs,i*32+24)
        if offset>len(refs) or count*2>len(refs)-offset or offset%2:
            raise ValueError('Invalid microchunk references')
        ids=list(struct.unpack_from(f'<{count}H',refs,offset))
        if any(v>=mchunks for v in ids):
            raise ValueError('Unknown microchunk')
        if label>=len(labels) or labels.find(b'\0',label)<0:
            raise ValueError('Unterminated chunk label')
        result.append(dict(id=i,microchunks=ids,required_locus=attrs[i*32+2]))
    ranges=[]
    for i in range(mchunks):
        loc,size=struct.unpack_from('<QQ',physical,i*16)
        ranges.append(dict(id=i,package_offset=loc&((1<<48)-1),image=(loc>>56)&15,size=size&((1<<48)-1)))
    return dict(chunks=chunks,microchunks=mchunks,scenarios=scenarios,chunk_references=result,
                package_ranges=ranges,availability='unverified: package offsets do not map directly to extracted paths')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('game',type=Path)
    p.add_argument('--out',type=Path,default=Path('out/tlg/playgo-inspection.json'))
    a=p.parse_args();report=inspect((a.game/'sce_sys/playgo-chunk.dat').read_bytes())
    a.out.parent.mkdir(parents=True,exist_ok=True)
    a.out.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(f"PlayGo: {report['chunks']} chunks, {report['microchunks']} microchunks; availability unverified")
