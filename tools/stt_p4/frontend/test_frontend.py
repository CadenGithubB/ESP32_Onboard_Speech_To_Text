#!/usr/bin/env python3
"""Host compiler/sanitizer checks and numerical parity with independent Torch frontend."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import re
import numpy as np
import soundfile as sf
import torch
from reference_frontend import LOG_GUARD, features_pcm16

HERE=Path(__file__).resolve().parent
PRODUCTION=HERE.parents[2]/'firmware/components/hw1_stt/stt'
# Fixtures whose cells sit far below the frame peak (dither-only silence, a pure tone's leakage
# bins). The 2^-30 guard no longer hides float32 rounding there: at 2^-30 the C++ real-FFT split
# and torch.stft each differ from a float64 frontend by up to ~4e-3 on sine480000, and from each
# other by up to 2.6e-3 (silence1600 dithered 3.4e-3); at 2^-24 both stayed under 6e-4. Speech
# and broadband fixtures keep the original thresholds and must give identical int8 inputs.
FLOOR_FIXTURES={'silence320','silence1600','near_silence1601','sine480000'}
SIGNAL_TOLERANCE=(6e-4,2e-5,0.0)   # max, mean abs error; fraction of int8 (exp -3) inputs differing
FLOOR_TOLERANCE=(5e-3,2e-4,2e-3)

def pinned_state():
    """Window and mel filters exactly as compiled (quartznet_constants.h), verified against the hashes
    of the pinned NVIDIA checkpoint tensors in constants-manifest.json; no checkpoint file needed."""
    manifest=json.loads((HERE/'constants-manifest.json').read_text())
    header=(PRODUCTION/'quartznet_constants.h').read_bytes()
    if hashlib.sha256(header).hexdigest()!=manifest['header_sha256']: raise ValueError('quartznet_constants.h differs from the manifest')
    text=header.decode()
    def array(name,parse):
        body=re.search(r'\b'+name+r'\[\d+\] = \{([^}]*)\}',text).group(1)
        return [parse(x.strip()) for x in body.split(',') if x.strip()]
    hexfloat=lambda x: float.fromhex(x.rstrip('f'))
    window=np.array(array('window',hexfloat),np.float32); weights=np.array(array('mel_weight',hexfloat),np.float32)
    start,count,offset=(array(n,int) for n in ('mel_start','mel_count','mel_offset'))
    mel=np.zeros((64,257),np.float32)
    for m in range(64): mel[m,start[m]:start[m]+count[m]]=weights[offset[m]:offset[m+1]]
    for name,values in (('window_sha256_f32le',window),('mel_sha256_f32le',mel)):
        if hashlib.sha256(values.astype('<f4').tobytes()).hexdigest()!=manifest[name]: raise ValueError('Constants differ from the pinned checkpoint: '+name)
    return {'preprocessor.featurizer.window':torch.from_numpy(window),'preprocessor.featurizer.fb':torch.from_numpy(mel)[None]}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures',type=Path,default=HERE.parents[1]/'speech_portable/private/stt-quartznet/fixtures',
                        help='Folder with the public speech clips 0.wav and 1.wav (any checkpoint folder\'s fixtures/)')
    parser.add_argument('--sanitize',action='store_true')
    parser.add_argument('--output',type=Path,default=HERE.parent/'private/frontend')
    args=parser.parse_args()
    torch.set_num_threads(1)
    state=pinned_state()
    args.output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='hw1-quartz-frontend-') as directory:
        binary=Path(directory)/'frontend'
        cmd=['clang++','-std=c++17','-O2','-ffp-contract=off','-Wall','-Wextra','-Werror',str(PRODUCTION/'quartznet_frontend.cpp'),str(HERE/'test_driver.cpp'),'-I'+str(PRODUCTION),'-o',str(binary)]
        if args.sanitize: cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True)
        subprocess.run([str(binary)],check=True)
        rng=np.random.default_rng(921)
        cases=[('silence320',np.zeros(320,np.int16)),('silence1600',np.zeros(1600,np.int16)),
               ('near_silence1601',rng.integers(-1,2,1601,dtype=np.int16)),
               ('random321',rng.integers(-32768,32768,321,dtype=np.int16)),
               ('random1599',rng.integers(-30000,30001,1599,dtype=np.int16)),
               ('impulse1600',np.array([32767]+[0]*1599,dtype=np.int16)),
               ('sine480000',np.round(np.sin(np.arange(480000)*0.08)*16000).astype(np.int16))]
        for name in ['0.wav','1.wav']:
            pcm,rate=sf.read(args.fixtures/name,dtype='int16')
            if rate!=16000 or pcm.ndim!=1: raise ValueError('Unexpected fixture format')
            cases.append((name[:-4],pcm))
        results=[]
        for name,pcm in cases:
            raw=args.output/(name+'.pcm16le'); out=args.output/(name+'.features.f32le')
            raw.write_bytes(pcm.astype('<i2').tobytes())
            subprocess.run([str(binary),str(raw),str(out)],check=True,capture_output=True)
            actual=np.frombuffer(out.read_bytes(),dtype='<f4').reshape(-1,64)
            reference=features_pcm16(pcm,state)
            error=np.abs(actual-reference)
            quantized=lambda x: np.clip(np.rint(x*8),-128,127)   # v3 packages: input exponent -3
            int8_diff=int(np.count_nonzero(quantized(actual)!=quantized(reference)))
            tolerance=FLOOR_TOLERANCE if name in FLOOR_FIXTURES else SIGNAL_TOLERANCE
            item={'fixture':name,'samples':len(pcm),'frames':len(actual),'max_abs_error':float(error.max()),
                  'mean_abs_error':float(error.mean()),'p99_abs_error':float(np.quantile(error,0.99)),
                  'int8_exp3_differences':int8_diff,'tolerance':tolerance,
                  'pcm_sha256':hashlib.sha256(raw.read_bytes()).hexdigest(),'features_sha256':hashlib.sha256(out.read_bytes()).hexdigest()}
            results.append(item); print(json.dumps(item),flush=True)
            if (not np.isfinite(actual).all() or error.max()>tolerance[0] or error.mean()>tolerance[1]
                    or int8_diff>tolerance[2]*actual.size):
                raise AssertionError('Frontend numeric parity exceeded tolerance: '+name)
            np.save(args.output/(name+'.reference.npy'),reference)
        record={'schema':2,'scope':'Host-only portable frontend parity, not P4 timing','sanitize':args.sanitize,'dither_seed':'0x12345678',
                'log_guard':LOG_GUARD,'results':results}
        (args.output/'results.json').write_text(json.dumps(record,indent=2)+'\n')
if __name__=='__main__': main()
