"""Zhiyi IME: writes a translation model's part-of-speech head (pos_head/ of the safetensors
release) into a copy of its GGUF as metadata (zhiyi.pos_head.*), read by the patched llama-server
(server-pos.h). Uses gguf-py from the llama.cpp folder.

    python add_pos_head.py <llama.cpp folder> <model.gguf> <pos_head folder> <out.gguf>
"""
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(sys.argv[1], 'gguf-py'))
import gguf  # noqa: E402
from gguf.scripts.gguf_new_metadata import MetadataDetails, copy_with_new_metadata  # noqa: E402


def safetensors(path):
    with open(path, 'rb') as f:
        n = struct.unpack('<Q', f.read(8))[0]
        header = json.loads(f.read(n))
        data = f.read()
    out = {}
    for name, info in header.items():
        if name == '__metadata__':
            continue
        assert info['dtype'] == 'F32', (name, info['dtype'])
        a, b = info['data_offsets']
        out[name] = np.frombuffer(data[a:b], dtype='<f4').reshape(info['shape'])
    return out


def main(model, head_dir, out):
    cfg = json.load(open(os.path.join(head_dir, 'pos_head.json'), encoding='utf-8'))
    t = safetensors(os.path.join(head_dir, 'pos_head.safetensors'))
    # hidden_states i (transformers, 0 = embeddings) is the output of layer i - 1: l_out-(i-1)
    layers = [int(name.split('-')[1]) for name in cfg['llama_cpp_tensors']]
    assert layers == [i - 1 for i in cfg['hidden_states']], (layers, cfg['hidden_states'])
    tags = [tag['upos'] for tag in sorted(cfg['tags'], key=lambda tag: tag['id'])]
    assert t['fc2.bias'].shape == (len(tags),) and t['mean'].shape[0] == len(layers)

    def floats(a):
        return MetadataDetails(gguf.GGUFValueType.ARRAY, [float(x) for x in a.ravel()],
                               sub_type=gguf.GGUFValueType.FLOAT32)

    meta = {
        'zhiyi.pos_head.model': MetadataDetails(gguf.GGUFValueType.STRING, cfg['model']),
        'zhiyi.pos_head.layers': MetadataDetails(gguf.GGUFValueType.ARRAY, layers, sub_type=gguf.GGUFValueType.INT32),
        'zhiyi.pos_head.tags': MetadataDetails(gguf.GGUFValueType.ARRAY, tags, sub_type=gguf.GGUFValueType.STRING),
        'zhiyi.pos_head.mean': floats(t['mean']),
        'zhiyi.pos_head.std': floats(t['std']),
        'zhiyi.pos_head.fc1.weight': floats(t['fc1.weight']),
        'zhiyi.pos_head.fc1.bias': floats(t['fc1.bias']),
        'zhiyi.pos_head.fc2.weight': floats(t['fc2.weight']),
        'zhiyi.pos_head.fc2.bias': floats(t['fc2.bias']),
    }
    reader = gguf.GGUFReader(model, 'r')
    arch = reader.get_field(gguf.Keys.General.ARCHITECTURE).contents()
    writer = gguf.GGUFWriter(out, arch=arch, endianess=reader.endianess)
    alignment = reader.get_field(gguf.Keys.General.ALIGNMENT)
    if alignment is not None:
        writer.data_alignment = alignment.contents()
    copy_with_new_metadata(reader, writer, meta, [])
    print('wrote', out, os.path.getsize(out))


if __name__ == '__main__':
    main(*sys.argv[2:5])
