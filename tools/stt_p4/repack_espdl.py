#!/usr/bin/env python3
"""Rewrite an EDL2 .espdl so that what ESP-DL reads only while it builds a model lies at the end of the file.

  repack_espdl.py IN.espdl OUT.espdl [--report OUT.json]

ESP-DL runs a model in place: its parameter tensors point at the weights inside the file image, which
therefore stays in RAM for as long as the model lives. The exporter interleaves the weights with data
that is copied out during the build and never read again: per-channel exponents (int64 in the schema:
1.2 MB for Citrinet-384, against 150 KB once copied), each parameter's shape and name, and the embedded
test input and output. This tool writes the same model with that data last:

  [16-byte EDL2 header][tables, nodes, attributes, value info][weights][build-only data]

so a loader that has built its model can shrink the image to `keep_bytes` (a heap realloc in place) and
get the rest back. Nothing else changes: every field of every table has the same value, the weights
keep their 16-byte alignment, and the output is checked against the input field by field.

Build-only data is: for every graph initializer its exponents, dims, name and doc string; the graph's
test inputs and outputs entirely. Everything else, including tensors held in node attributes, stays in
the kept part. A model that keeps parameters outside raw_data (float_data, int32_data, ...) is refused.

Needs esp-ppq (its generated FlatBuffers classes) and numpy, as export.py does.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import numpy as np

RAW_SLOT = 20          # vtable offset of Tensor.raw_data (field 8)
NUMERIC = (('dims', 'Dims', np.int64), ('floatData', 'FloatData', np.float32), ('int32Data', 'Int32Data', np.int32),
           ('int64Data', 'Int64Data', np.int64), ('doubleData', 'DoubleData', np.float64),
           ('uint64Data', 'Uint64Data', np.uint64), ('exponents', 'Exponents', np.int64))
ADD_ORDER = ('dims', None, 'floatData', 'int32Data', 'stringData', 'int64Data', 'name', 'docString', 'rawData',
             'externalData', None, 'doubleData', 'uint64Data', 'exponents')   # TensorT.Pack's order; None: a scalar


class Raw:
    """A tensor's raw_data as bytes (the generated object API makes one Python object per 16 bytes)."""
    __slots__ = ('data',)

    def __init__(self, data):
        self.data = bytes(data)
        if len(self.data) % 16:
            raise ValueError('raw_data is not a whole number of 16-byte blocks')


def classes():
    from esp_ppq.parser.espdl import helper  # noqa: F401  (puts the generated FlatBuffers package on the path)
    import flatbuffers
    from FlatBuffers.Dl import Model, Tensor
    return flatbuffers, Model, Tensor


def split(raw):
    if raw[:4] != b'EDL2' or struct.unpack_from('<II', raw, 4) != (0, len(raw) - 16):
        raise ValueError('expected an unencrypted EDL2 file whose header gives its length')
    return raw[:16], raw[16:]


def unpack(buf):
    """ModelT of a flatbuffer, with every TensorT.rawData a Raw."""
    _, Model, Tensor = classes()
    original = Tensor.TensorT._UnPack

    class NoRaw:
        def __init__(self, tensor): self._tensor = tensor
        def __getattr__(self, name): return getattr(self._tensor, name)
        def RawDataIsNone(self): return True

    def light(self, tensor):
        original(self, NoRaw(tensor))
        o = tensor._tab.Offset(RAW_SLOT)
        if o:
            start, count = tensor._tab.Vector(o), tensor._tab.VectorLen(o)
            self.rawData = Raw(tensor._tab.Bytes[start:start + 16 * count])

    Tensor.TensorT._UnPack = light
    try:
        return Model.ModelT.InitFromObj(Model.Model.GetRootAs(buf, 0))
    finally:
        Tensor.TensorT._UnPack = original


def same(a, b, where='model'):
    """Raise unless two unpacked values are equal, field by field."""
    if isinstance(a, Raw) or isinstance(b, Raw):
        if not (isinstance(a, Raw) and isinstance(b, Raw) and a.data == b.data):
            raise ValueError(f'{where}: raw data differs')
    elif isinstance(a, np.ndarray) or isinstance(b, np.ndarray):
        x, y = np.asarray(a), np.asarray(b)
        if x.dtype != y.dtype or x.shape != y.shape or x.tobytes() != y.tobytes():
            raise ValueError(f'{where}: array differs')
    elif isinstance(a, (list, tuple)):
        if not isinstance(b, (list, tuple)) or len(a) != len(b):
            raise ValueError(f'{where}: length differs')
        for i, (x, y) in enumerate(zip(a, b)):
            same(x, y, f'{where}[{i}]')
    elif hasattr(a, '__dict__'):
        if type(a) is not type(b) or vars(a).keys() != vars(b).keys():
            raise ValueError(f'{where}: type differs')
        for key in vars(a):
            same(getattr(a, key), getattr(b, key), f'{where}.{key}')
    elif type(a) is not type(b) or a != b:
        raise ValueError(f'{where}: {a!r} != {b!r}')


def repack(raw):
    """(repacked file bytes, report dict). The input is not modified."""
    flatbuffers, Model, Tensor = classes()
    header, buf = split(raw)
    model = unpack(buf)
    graph = model.graph
    initializers = list(graph.initializer or [])
    tests = list(getattr(graph, 'testInputsValue', None) or []) + list(getattr(graph, 'testOutputsValue', None) or [])
    for t in initializers:
        if any(getattr(t, f) is not None for f in ('floatData', 'int32Data', 'stringData', 'int64Data',
                                                   'externalData', 'doubleData', 'uint64Data')):
            raise ValueError(f'initializer {t.name!r} keeps data outside raw_data; not supported')
        if t.rawData is None:
            raise ValueError(f'initializer {t.name!r} has no raw_data')

    builder = flatbuffers.Builder(len(buf) + (1 << 20))
    made = {}       # id(TensorT) -> {field: offset} of the vectors and strings created ahead of its table

    def raw_vector(data):
        builder.StartVector(16, len(data) // 16, 16)
        builder.head = builder.Head() - len(data)
        builder.Bytes[builder.Head():builder.Head() + len(data)] = data
        return builder.EndVector()

    def leaves(t, fields):
        out = made.setdefault(id(t), {})
        for name in fields:
            value = getattr(t, name)
            if value is None or name in out:
                continue
            if name == 'rawData':
                out[name] = raw_vector(value.data)
            elif name in ('name', 'docString'):
                out[name] = builder.CreateString(value)
            elif name in ('stringData', 'externalData'):
                raise ValueError(f'tensor {t.name!r}: {name} is not supported')
            else:
                dtype = next(d for f, _, d in NUMERIC if f == name)
                out[name] = builder.CreateNumpyVector(np.ascontiguousarray(value, dtype=dtype))
        return out

    everything = tuple(f for f in ADD_ORDER if f)
    build_only = ('exponents', 'dims', 'name', 'docString')

    def pack(self, b):
        assert b is builder
        o = leaves(self, everything)
        Tensor.TensorStart(b)
        scalars = iter((lambda: Tensor.TensorAddDataType(b, self.dataType),
                        lambda: Tensor.TensorAddDataLocation(b, self.dataLocation)))
        for name in ADD_ORDER:
            if name is None:
                next(scalars)()
            elif name in o:
                getattr(Tensor, 'TensorAdd' + name[0].upper() + name[1:])(b, o[name])
        return Tensor.TensorEnd(b)

    # A flatbuffer is written back to front: what is created first ends up last in the file.
    for t in tests:
        leaves(t, everything)
    for t in initializers:
        leaves(t, build_only)
    tail = builder.Offset()                      # bytes from the end of the buffer
    for t in reversed(initializers):             # initializer 0's weights lowest, as the exporter has them
        leaves(t, ('rawData',))
    weights = builder.Offset()
    original_pack = Tensor.TensorT.Pack
    Tensor.TensorT.Pack = pack
    try:
        builder.Finish(model.Pack(builder))
    finally:
        Tensor.TensorT.Pack = original_pack
    out = bytes(builder.Output())
    keep = (16 + len(out) - tail + 15) & ~15     # file bytes a built model still reads, to a 16-byte boundary

    # Checks on the result itself: same content, weights aligned and inside the kept part, build-only data after it.
    again = unpack(out)
    same(model, again)
    g = Model.Model.GetRootAs(out, 0).Graph()
    first_weight, last_weight = len(out), 0
    for i in range(g.InitializerLength()):
        t = g.Initializer(i)
        o = t._tab.Offset(RAW_SLOT)
        start, count = t._tab.Vector(o), t._tab.VectorLen(o)
        if start % 16 or start + 16 * count > len(out) - tail or start - 4 < len(out) - weights:
            raise ValueError(f'initializer {i}: weights misplaced')
        first_weight, last_weight = min(first_weight, start - 4), max(last_weight, start + 16 * count)
        for slot, what in ((4, 'dims'), (30, 'exponents')):
            v = t._tab.Offset(slot)
            if v and t._tab.Vector(v) - 4 < len(out) - tail:
                raise ValueError(f'initializer {i}: {what} inside the kept part')
    if buf[4:8] != out[4:8] and (buf[4:8].isalnum() or out[4:8].isalnum()):
        raise ValueError('file identifier changed')
    result = header[:8] + struct.pack('<I', len(out)) + header[12:16] + out
    report = {
        'input': {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()},
        'output': {'bytes': len(result), 'sha256': hashlib.sha256(result).hexdigest()},
        'keep_bytes': keep,
        'build_only_bytes': len(result) - keep,
        'layout': {'tables': 16 + first_weight, 'weights': last_weight - first_weight, 'build_only': tail},
        'initializers': len(initializers), 'test_tensors': len(tests), 'nodes': len(graph.node or []),
    }
    return result, report


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--report', type=Path)
    a = p.parse_args()
    result, report = repack(a.input.read_bytes())
    a.output.write_bytes(result)
    text = json.dumps(report, indent=2) + '\n'
    if a.report:
        a.report.write_text(text)
    sys.stdout.write(text)


if __name__ == '__main__':
    main()
