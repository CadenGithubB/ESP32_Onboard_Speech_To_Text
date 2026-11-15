#!/usr/bin/env python3
"""Level-robust onset (decision_pre_emphasis): unit cases U1-U16 under ASan/UBSan, plus mutants.

1. Builds tests/segmenter_decision_test.cpp against the real header with -Werror and
   -fsanitize=address,undefined (UBSan reports are fatal) and runs every case.
2. When git has commit 1a51eac (the header before the option existed), builds the option-0
   diagnostics hash against that header and checks it equals the golden value in the test.
   The option-on (production) hash of U13 is likewise rebuilt against the header from before
   the modulation gate (private/segmenter_replay/stt_segmenter.before-gate.h, ignored by git)
   when that copy exists.
3. Builds header mutants that each revert ONE defect fix (the three HardwareOne defects, the
   gain-percentile graft, the three review fixes of 12 Nov, the two noise controls of 13 Nov and
   their two review fixes: the hold's raw latch ends with a continuation that has no raw evidence,
   and decision_modulated() reports the pre path's vote while the gate is off)
   and checks that the case written for that fix fails on it (a test that passes on its mutant
   does not count).
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
component = root / "firmware/components/hw1_stt"
source = root / "tests/segmenter_decision_test.cpp"
header = component / "stt/stt_segmenter.h"
CXX = os.environ.get("CXX", "c++")
BASE_COMMIT = "1a51eac"
BEFORE_GATE = root / "private/segmenter_replay/stt_segmenter.before-gate.h"

# (name, case that must fail, text in stt_segmenter.h, replacement[, more (text, replacement) pairs])
MUTANTS = [
    ("a: decision level from floor(sqrt) RMS", "U3",
     "        raw_e_=centered*kSegmentEnergyUnit/nn;",
     "        raw_e_=centered*kSegmentEnergyUnit/nn;\n"
     "        { const uint64_t q=integer_sqrt(static_cast<uint32_t>(std::min<uint64_t>(dec_e_/kSegmentEnergyUnit,0xffffffffu)));"
     " dec_e_=q*q*kSegmentEnergyUnit; }"),
    ("b: gain learned only during calibration (frozen after boot)", "U4",
     "        if(!active_)learn_gain();",
     "        if(!active_ && !calibrated_)learn_gain();"),
    ("c: quiet-cut carry compares raw energy with the decision threshold", "U6",
     "if((dec_votes && frame_energy_d(at)/(F*F)>=thr_d_) ||\n"
     "                   (raw_votes && frame_energy(at)*kSegmentEnergyUnit/(F*F)>=thr_r_))carry_voice_+=F;",
     "if((dec_votes || raw_votes) && frame_energy(at)*kSegmentEnergyUnit/(F*F)>=thr_d_)carry_voice_+=F;"),
    ("graft: gain percentile follows noise_percentile", "U9",
     "percentile_energy(idle_r_,idle_count_,kDecisionGainPercentile)",
     "percentile_energy(idle_r_,idle_count_,config_.noise_percentile)"),
    ("review: constant frames enter the gain window and a zero quartile maps to kSegmentGainMax", "U11",
     "        if((!calibrated_ || !active_) && raw_e_) {",
     "        if(!calibrated_ || !active_) {",
     [("        if(!d)return;\n", "        if(!d) { set_gain(kSegmentGainMax); return; }\n")]),
    ("review: constant frames enter the gain window (zero-quartile guard kept)", "U11",
     "        if((!calibrated_ || !active_) && raw_e_) {",
     "        if(!calibrated_ || !active_) {"),
    ("review: no re-seed after decision-only hard limits", "U12",
     "        if(config_.decision_pre_emphasis && segment_.end==SegmentEnd::HardLimit)after_hard_cut();\n",
     ""),
    ("gate: the pre-emphasised level votes whether or not it is modulated", "U14",
     "    bool pre_votes() const { return !config_.decision_modulation_ratio_pct || modulated_; }",
     "    bool pre_votes() const { return true; }"),
    ("hold: raw votes count before the raw level has latched", "U15",
     "    bool raw_votes_count() const { return !config_.decision_pre_hold_requires_raw || raw_heard_; }",
     "    bool raw_votes_count() const { return true; }"),
    ("hold review: the raw latch is kept through the whole hard-cut chain", "U15",
     "        if(!raw_onset_)raw_heard_=false;\n",
     ""),
    ("gate review: decision_modulated() reads the gate's ring state even while the gate is off", "U14",
     "    bool decision_modulated() const { return config_.decision_pre_emphasis && pre_votes(); }",
     "    bool decision_modulated() const { return modulated_; }"),
]


def build(include, out, *extra, sanitize=True):
    flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run([CXX, *flags, *extra, "-I" + str(include), str(source), "-o", str(out)], check=True)


with tempfile.TemporaryDirectory(prefix="segmenter-decision-") as tmp:
    tmp = Path(tmp)

    # 1. All cases on the real header.
    binary = tmp / "decision"
    build(component, binary)
    subprocess.run([str(binary)], check=True)

    # 2. Option 0 equals the header from before the option, recomputed from git when available.
    golden = int(re.search(r"kGoldenDiagHash=(\d+)ull", source.read_text()).group(1))
    hashes = {}
    for name, include in (("current", component), ("base", tmp / "base")):
        if name == "base":
            files = {"stt/stt_segmenter.h": "firmware/components/hw1_stt/stt/stt_segmenter.h",
                     "Audio_VadPolicy.h": "firmware/components/hw1_stt/Audio_VadPolicy.h"}
            try:
                for dest, path in files.items():
                    text = subprocess.run(["git", "-C", str(root), "show", f"{BASE_COMMIT}:{path}"],
                                          capture_output=True, text=True, check=True).stdout
                    (include / dest).parent.mkdir(parents=True, exist_ok=True)
                    (include / dest).write_text(text)
            except (subprocess.CalledProcessError, FileNotFoundError):
                print(f"option-0 hash vs {BASE_COMMIT}: skipped (commit not available); the golden value is still checked by U1")
                continue
        out = tmp / f"hash_{name}"
        build(include, out, "-DSEGMENTER_DIAG_HASH_ONLY", sanitize=False)
        hashes[name] = int(subprocess.run([str(out)], capture_output=True, text=True, check=True).stdout)
    assert hashes["current"] == golden, (hashes, golden)
    if "base" in hashes:
        assert hashes["base"] == golden, (hashes, golden)
        print(f"option-0 diagnostics hash equals the {BASE_COMMIT} header's: {golden}")

    # 2b. With the noise controls off, the option-on hash equals the header from before them.
    prod_golden = int(re.search(r"kGoldenProdHash=(\d+)ull", source.read_text()).group(1))
    prod = {}
    for name, include in (("current", component), ("before-gate", tmp / "before_gate")):
        if name == "before-gate":
            if not BEFORE_GATE.exists():
                print(f"option-on hash vs the pre-gate header: skipped ({BEFORE_GATE} not present); U13 still checks the golden value")
                continue
            shutil.copytree(component, include)
            shutil.copyfile(BEFORE_GATE, include / "stt/stt_segmenter.h")
        out = tmp / f"prod_{name}"
        build(include, out, "-DSEGMENTER_PROD_HASH_ONLY", "-Wno-unused-function", "-Wno-unused-const-variable", sanitize=False)
        prod[name] = int(subprocess.run([str(out)], capture_output=True, text=True, check=True).stdout)
    assert prod["current"] == prod_golden, (prod, prod_golden)
    if "before-gate" in prod:
        assert prod["before-gate"] == prod_golden, (prod, prod_golden)
        print(f"option-on hash with the noise controls off equals the pre-gate header's: {prod_golden}")

    # 3. Each defect fix is load-bearing for its case.
    original = header.read_text()
    for name, case, old, new, *more in MUTANTS:
        text = original
        for o, n in [(old, new), *(more[0] if more else [])]:
            assert text.count(o) == 1, f"mutant '{name}': text not found exactly once in stt_segmenter.h: {o!r}"
            text = text.replace(o, n)
        mutant = tmp / "mutant"
        if mutant.exists():
            shutil.rmtree(mutant)
        shutil.copytree(component, mutant)
        (mutant / "stt/stt_segmenter.h").write_text(text)
        out = tmp / "mutant_test"
        build(mutant, out, sanitize=False)
        r = subprocess.run([str(out), case], capture_output=True, text=True)
        assert r.returncode != 0, f"mutant '{name}' was NOT detected by {case}:\n{r.stdout}"
        failed = [line.strip() for line in r.stdout.splitlines() if "FAILED " in line]
        print(f"mutant {name}: detected by {case} ({'; '.join(failed)})")

print("segmenter decision checks passed")
