#!/usr/bin/env python3
"""Derive the Citrinet-384 graph's minimal ESP-DL 3.3.12 compile requirements (citrinet.compile_req.yml).

  generate_citrinet_compile_requirements.py --model citrinet.espdl --esp-dl <patched esp-dl> --engine <stt folder> [--check]

As generate_compile_requirements.py does for QuartzNet: reads the actual EDL2 Conv attributes and the pinned
runtime's Conv selector (spec/select/Conv.yml) to list the kernels the graph selects, plus the graph's
operators (Citrinet adds GlobalAveragePool, Sigmoid and Mul for squeeze-and-excitation) and RequantizeLinear
(TensorBase::assign links it). The model must be the one pinned in stt_model_identity_citrinet384.h. ESP-DL's
own finalizer then verifies the list alone and in union with ESP-SR's when that component is present.
"""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

import yaml

HERE = Path(__file__).resolve().parent
EXPECTED_OPS = ['Add', 'Conv', 'GlobalAveragePool', 'Mul', 'Relu', 'Sigmoid']


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def shape(value): return [int(d.value.dimValue) for d in value.valueInfoType.value.shape.dim]


def attribute(attr):
    if attr.attrType == 2: return int(attr.i.i)
    if attr.attrType == 3: return bytes(attr.s).decode()
    if attr.attrType == 7: return [int(x) for x in attr.ints]
    raise ValueError('Unsupported required attribute: ' + attr.name.decode())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--model', type=Path, required=True)
    p.add_argument('--esp-dl', type=Path, required=True)
    p.add_argument('--engine', type=Path, required=True)
    p.add_argument('--check', action='store_true')
    a = p.parse_args()
    dl, output = a.esp_dl, a.engine / 'citrinet.compile_req.yml'
    provenance = a.engine / 'citrinet-compile-requirements-provenance.json'
    if str(yaml.safe_load((dl / 'idf_component.yml').read_text())['version']) != '3.3.12':
        raise ValueError('Only the inspected ESP-DL 3.3.12 selector is supported')
    model_sha = sha(a.model)
    identity = (a.engine / 'stt_model_identity_citrinet384.h').read_text()
    pinned = re.search(r'kRawSha\[32\]\s*=\s*\{([^}]+)\}', identity).group(1)
    if bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', pinned)).hex() != model_sha:
        raise ValueError('Model differs from the pinned Citrinet identity')
    from esp_ppq.parser.espdl import helper  # noqa: F401
    from FlatBuffers.Dl.Model import Model, ModelT
    raw = a.model.read_bytes()
    if raw[:4] != b'EDL2' or struct.unpack_from('<II', raw, 4) != (0, len(raw) - 16):
        raise ValueError('Expected bounded unencrypted EDL2')
    graph = ModelT.InitFromObj(Model.GetRootAs(raw, 16)).graph
    shapes = {v.name: shape(v) for v in [*graph.input, *graph.output, *graph.valueInfo]}
    tensors = {t.name: t for t in graph.initializer}
    rules = yaml.safe_load((dl / 'spec/select/Conv.yml').read_text())
    catalog = yaml.safe_load((dl / 'spec/kernels.yml').read_text())
    graph_ops = sorted(set(n.opType.decode() for n in graph.node))
    if graph_ops != EXPECTED_OPS:
        raise ValueError('Review changed graph operator set: ' + str(graph_ops))
    ops = sorted(set(graph_ops + ['RequantizeLinear']))
    spec = importlib.util.spec_from_file_location('espdl_compile_finalize', dl / 'cmake/compile_finalize.py')
    selector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(selector)
    kernels, nodes = set(), []
    for node in graph.node:
        if node.opType != b'Conv':
            continue
        attr = {x.name.decode(): attribute(x) for x in node.attribute}
        inp, out, filt = shapes[node.input[0]], shapes[node.output[0]], tensors[node.input[1]]
        dims = [int(x) for x in filt.dims]
        if len(inp) != 4 or len(out) != 4 or len(dims) != 4 or attr['quant_type'] != 'S8':
            raise ValueError('Expected S8 NHWC Conv2D')
        kh, kw = dims[:2]
        if [kh, kw] != attr['kernel_shape']:
            raise ValueError('Filter dimensions disagree with kernel_shape')
        group, cin, cout = attr['group'], inp[-1], out[-1]
        if group not in (1, cin):
            raise ValueError('Only regular/depthwise convolution supported')
        signature = {'target': 'pie_v2', 'dtype': 's8', 'group': '1' if group == 1 else 'dw',
                     'kshape': '11' if (kh, kw) == (1, 1) else '33' if (kh, kw) == (3, 3) else 'hw',
                     'bias': len(node.input) == 3, 'act': attr['activation'].lower(),
                     'per_ch': len(filt.exponents) > 1, 'c_align': cin % 16 == 0 and cout % 16 == 0}
        matches = [r for r in rules['rules'] if all(r.get(k) == v for k, v in signature.items())]
        if len(matches) != 1:
            raise ValueError('Ambiguous/missing Conv selector: ' + str(signature))
        slots = {slot: matches[0][slot] for slot in rules['slots']['pie_v2'] if matches[0].get(slot)}
        if not slots.get('body'):
            raise ValueError('No body kernel')
        kernels.update(slots.values())
        nodes.append({'node': node.name.decode(), 'input_channels': cin, 'output_channels': cout,
                      'filter_shape': dims, 'strides': attr.get('strides'), 'signature': signature, 'slots': slots})
    if kernels - set(catalog['symbols']):
        raise ValueError('Selector names unknown kernel')
    encoded = ('# Generated by tools/stt_p4/generate_citrinet_compile_requirements.py.\n'
               '# Frozen raw model SHA256: ' + model_sha + '\n'
               '# ESP-DL 3.3.12; actual Conv2D attributes, PIEv2 int8 channel alignment.\n'
               '# Runtime T may vary; channels/filter shapes and selected kernels do not.\n'
               '# RequantizeLinear supports Model/TensorBase::assign; it is not a graph node.\n'
               'kernel_abi: ' + str(int(catalog['kernel_abi'])) + '\nops:\n' + ''.join('  - ' + x + '\n' for x in ops)
               + 'kernels:\n' + ''.join('  - ' + x + '\n' for x in sorted(kernels)))
    if a.check:
        if output.read_text() != encoded:
            raise SystemExit('Committed Citrinet compile requirements are stale')
    else:
        output.write_text(encoded)
    sr = dl.parent / 'espressif__esp-sr/tool/esp-sr-req.yml'
    checks = [[output]] + ([[sr, output]] if sr.exists() else [])
    with tempfile.TemporaryDirectory(prefix='hw1-citrinet-compile-req-') as temp:
        tmp = Path(temp)
        for requirements in checks:
            command = [sys.executable, str(dl / 'cmake/compile_finalize.py'), '--ops', str(dl / 'spec/ops.yml'),
                       '--kernels', str(dl / 'spec/kernels.yml'), '--conv-yml', str(dl / 'spec/select/Conv.yml'),
                       '--target', 'esp32p4', '--component-dir', str(dl), '--header', str(tmp / 'config.h'),
                       '--srcs', str(tmp / 'srcs.cmake'), '--req', *[str(x) for x in requirements]]
            subprocess.run(command, check=True, text=True)
            header = (tmp / 'config.h').read_text()
            if '#define DL_COMPILE_ALL 0' not in header:
                raise ValueError('Unexpected compile-all')
            merged_ops, merged_kernels, _ = selector.union_reqs(requirements)
            for name in merged_ops:
                if '#define ' + selector.macro_op(name) + ' 1' not in header:
                    raise ValueError('Missing merged op ' + name)
            for name in merged_kernels:
                if '#define ' + selector.macro_kernel(name) + ' 1' not in header:
                    raise ValueError('Missing merged kernel ' + name)
    report = {'schema': 1, 'esp_dl': '3.3.12', 'raw_model_sha256': model_sha, 'model_nodes': len(graph.node),
              'operator_counts': dict(Counter(n.opType.decode() for n in graph.node)), 'graph_ops': graph_ops,
              'kernels': sorted(kernels), 'requirements_sha256': hashlib.sha256(encoded.encode()).hexdigest(),
              'verified_with_esp_sr_union': sr.exists(), 'generator_sha256': sha(__file__),
              'source_sha256': {n: sha(dl / n) for n in ['spec/ops.yml', 'spec/kernels.yml', 'spec/select/Conv.yml',
                                                          'cmake/compile_finalize.py']},
              'convolutions': nodes}
    if a.check:
        if json.loads(provenance.read_text()) != report:
            raise SystemExit('Citrinet compile requirement provenance is stale')
    else:
        provenance.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'ops': ops, 'conv_nodes': len(nodes), 'kernels': sorted(kernels),
                      'esp_sr_union_checked': sr.exists(), 'check': a.check}, indent=2))


if __name__ == '__main__':
    main()
