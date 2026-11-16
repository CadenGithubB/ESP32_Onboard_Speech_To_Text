#!/usr/bin/env python3
"""Host replay harness (tools/segmenter_replay): the driver uses the example's settings, its
segments do not depend on the block size, and the coverage tool agrees with score.py's mapping."""
import ast
import json
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
tool = root / "tools/segmenter_replay"
main_cpp = (root / "firmware/examples/p4_eye_captions/main/main.cpp").read_text()
kconfig = (root / "firmware/examples/p4_eye_captions/main/Kconfig.projbuild").read_text()
stream_cpp = (root / "firmware/components/hw1_stt/stt/stt_stream.cpp").read_text()
SR = 16000


def constant(name):
    """A `constexpr ... name = expr;` of main.cpp as an integer."""
    return evaluate(re.search(r"constexpr\s+\w+\s+" + name + r"\s*=\s*([^;]+);", main_cpp).group(1))


def evaluate(expr):
    expr = expr.replace("hw1::stt::kSegmentAnalysisSamples", "160").replace("true", "1").replace("false", "0")
    for name in ("kSampleRate", "kPhraseSamples", "kReadSamples"):
        if name in expr:
            expr = expr.replace(name, str(constant(name)))
    assert re.fullmatch(r"[\d\s*+/()-]+", expr), expr
    return int(eval(expr.replace("/", "//")))  # digits and arithmetic only, checked above


def kconfig_default_off(symbol):
    """True when Kconfig.projbuild declares CONFIG_<symbol> as a bool with `default n`."""
    m = re.search(r"^config " + symbol + r"\n((?:[ \t]+.*\n|\n)*?)(?=^\S|\Z)", kconfig, re.M)
    assert m, f"main.cpp tests CONFIG_{symbol}, which Kconfig.projbuild does not declare"
    default = re.search(r"^\s+default\s+(\w+)", m.group(1), re.M)
    return bool(default) and default.group(1) == "n"


def default_build(text):
    """main.cpp without the `#if CONFIG_X ... #endif` blocks (not nested, no #else) whose option defaults to n."""
    def keep(m):
        return "" if kconfig_default_off(m.group(1)) else m.group(0)
    return re.sub(r"^#if CONFIG_(\w+)\n(?:(?!#if|#el|#endif).*\n)*?#endif\n", keep, text, flags=re.M)


def run(binary, *args, ok=True):
    r = subprocess.run([str(binary)] + [str(a) for a in args], capture_output=True, text=True)
    assert (r.returncode == 0) == ok, (args, r.returncode, r.stderr)
    return r


with tempfile.TemporaryDirectory(prefix="segmenter-replay-") as tmp:
    tmp = Path(tmp)
    binary = tmp / "segmenter_replay"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I" + str(root / "firmware/components/hw1_stt"), str(tool / "segmenter_replay.cpp"), "-o", str(binary),
    ], check=True)

    # 1. The driver's settings are main.cpp's `cfg.segmentation.*` lines over the struct defaults,
    #    the block is the microphone read size and the tail is the latency profile's.
    shown = json.loads(run(binary, "--print-config").stdout)
    expected = dict(shown["segmenter_defaults"])
    #    Lines under a Kconfig option that defaults to n are not part of the default build.
    assigned = re.findall(r"cfg\.segmentation\.(\w+)\s*=\s*([^;]+);", default_build(main_cpp))
    assert assigned, "no cfg.segmentation lines found in main.cpp"
    gated = set(re.findall(r"cfg\.segmentation\.(\w+)\s*=", main_cpp)) - {name for name, _ in assigned}
    assert gated == {"decision_modulation_ratio_pct", "decision_pre_hold_requires_raw"}, gated
    assert all(name in shown["segmenter_defaults"] for name in gated), gated
    for name, expr in assigned:
        assert name in expected, f"main.cpp sets {name}, which the driver does not list"
        expected[name] = evaluate(expr)
    assert shown["segmenter_config"] == expected, (shown["segmenter_config"], expected)
    assert shown["block"] == constant("kReadSamples")
    assert shown["tail_samples"] == evaluate(re.search(r"cfg\.tail_samples\s*=\s*([^;]+);", main_cpp).group(1))
    assert shown["inner"] == int(re.search(r"std::min<size_t>\((\d+), count - offset\)", stream_cpp).group(1))
    fields = run(binary, "--list-fields").stdout.split()
    assert fields == list(shown["segmenter_config"])

    # 2. A synthetic session: a quiet room, three phrases, a click, and 14 s of unbroken speech
    #    (longer than the 10 s phrase limit).
    rng = random.Random(20261112)
    plan = [(2.0, 0), (3.0, 300), (2.0, 0), (0.02, 900), (2.0, 0), (1.2, 200), (3.0, 0), (14.0, 250), (2.5, 0), (5.0, 120), (3.0, 0)]
    pcm, bursts, at = [], [], 0
    for seconds, level in plan:
        n = int(seconds * SR)
        if level and seconds > 0.1:
            bursts.append((at, at + n))
        pcm += [int(rng.gauss(0, level or 5)) + 40 for _ in range(n)]  # +40: a DC offset must not matter
        at += n
    raw = tmp / "raw_test.pcm"
    raw.write_bytes(struct.pack(f"<{len(pcm)}h", *pcm))
    (tmp / "raw_test.json").write_text('{"format":"s16le","channels":1,"rate":16000,"samples_dropped":0}')
    base = json.loads(run(binary, raw, "--sidecar", tmp / "raw_test.json").stdout)
    segs = base["segments"]
    assert base["samples"] == len(pcm) and base["sidecar_dropped"] == 0
    assert len(segs) >= 5 and base["hard_limit_cuts"] >= 1, segs
    assert base["hard_limit_cuts"] == sum(g["end_kind"] == "hard_limit" for g in segs)
    assert base["segments_covered_samples"] == sum(g["end"] - g["start"] for g in segs)
    for g, nxt in zip(segs, segs[1:] + [None]):
        assert set(g) >= {"n", "start", "end", "end_kind", "continuation", "voiced_end", "new_voiced", "decode_end"}
        assert g["start"] < g["voiced_end"] <= g["end"] and g["start"] < g["decode_end"] <= g["end"]
        assert g["end"] - g["start"] <= base["segmenter_config"]["max_segment_samples"]
        assert nxt is None or nxt["start"] >= g["end"]
    for a, b in bursts:  # every phrase is inside segments
        assert sum(max(0, min(b, g["end"]) - max(a, g["start"])) for g in segs) == b - a, (a, b)

    # 3. Block-size invariance, option spellings, rejected input.
    for opts in (["--block", 1, "--inner", 0], ["--block", 7], ["--block", 160, "--inner", 0], ["--block", 1024, "--inner", 0],
                 ["--block", 4096, "--inner", 333], ["--chunk", len(pcm) + 5, "--inner", 0], ["--trace", tmp / "trace.csv"],
                 ["--set", "end_silence_samples=600ms"], ["--set", "min_segment_samples=4s"], ["--tail", 8000]):
        assert json.loads(run(binary, raw, *opts).stdout)["segments"] == segs, opts
    trace = (tmp / "trace.csv").read_text().splitlines()
    # The example turns the pre-emphasis decision on, which adds six energy columns; with it off the
    # trace keeps exactly the columns it had before the option existed.
    columns = "sample,rms,noise,threshold,open,active,peak,ready"
    energy = ",dec_e,dec_noise_e,dec_thr_e,raw_e,raw_thr_e,gain_q16"
    assert trace[0] == columns + (energy if base["segmenter_config"]["decision_pre_emphasis"] else "") and len(trace) == 1 + len(pcm) // 160
    ready_column = columns.split(",").index("ready")
    assert sum(line.split(",")[ready_column] == "1" for line in trace[1:]) == len(segs) - (segs[-1]["end_kind"] == "session_end")
    run(binary, raw, "--set", "decision_pre_emphasis=0", "--trace", tmp / "trace0.csv")
    trace0 = (tmp / "trace0.csv").read_text().splitlines()
    assert trace0[0] == columns and len(trace0) == len(trace)
    assert all(len(line.split(",")) == 8 for line in trace0[1:])
    # The noise controls (default off) add two trace columns only when one is on, and their arm is
    # block-size invariant too.
    gate = ["--set", "decision_modulation_ratio_pct=300", "--set", "decision_pre_hold_requires_raw=1"]
    run(binary, raw, *gate, "--trace", tmp / "trace_gate.csv")
    trace_gate = (tmp / "trace_gate.csv").read_text().splitlines()
    assert trace_gate[0] == columns + energy + ",modulated,raw_heard" and len(trace_gate) == len(trace)
    assert all(len(line.split(",")) == 16 and line.split(",")[14] in "01" for line in trace_gate[1:])
    gate_segs = json.loads(run(binary, raw, *gate).stdout)["segments"]
    for opts in (["--block", 1, "--inner", 0], ["--block", 4096, "--inner", 333], ["--trace", tmp / "trace_gate2.csv"]):
        assert json.loads(run(binary, raw, *gate, *opts).stdout)["segments"] == gate_segs, opts
    zero = ["--set", "decision_modulation_ratio_pct=0", "--set", "decision_pre_hold_requires_raw=0"]
    assert json.loads(run(binary, raw, *zero).stdout)["segments"] == segs
    full = json.loads(run(binary, raw, "--tail", 0).stdout)["segments"]
    assert all(g["decode_end"] == g["end"] for g in full)
    assert [{k: v for k, v in g.items() if k != "decode_end"} for g in full] == [{k: v for k, v in g.items() if k != "decode_end"} for g in segs]
    assert json.loads(run(binary, raw, "--set", "adaptive_speech_floor=24").stdout) == json.loads(run(binary, raw, "--speech-floor", 24).stdout)
    assert json.loads(run(binary, raw, "--set", "short_close_silence_samples=0").stdout) == json.loads(run(binary, raw, "--no-short-close").stdout)
    assert json.loads(run(binary, raw, "--set", "silence_blip_samples=160").stdout) == json.loads(run(binary, raw, "--blip-ms", 10).stdout)
    assert json.loads(run(binary, raw, "--set", "adaptive_speech_floor=5000").stdout)["segments"] == []
    for bad in (["--set", "no_such_field=1"], ["--set", "noise_percentile=300"], ["--set", "adaptive=2"], ["--set", "end_silence_samples=100"],
                ["--set", "adaptive_speech_floor"], ["--tail", 9600], ["--bogus"]):
        assert run(binary, raw, *bad, ok=False).returncode == 2, bad
    cuts = tmp / "cuts"
    cuts.mkdir()
    run(binary, raw, "--cut-dir", cuts)
    for g in segs:
        assert (cuts / f"seg_{g['n']}.pcm").read_bytes() == struct.pack(f"<{g['end'] - g['start']}h", *pcm[g["start"]:g["end"]])

    # 4. Coverage tool on a playlist laid over that session (raw = 1000 + 1.00001 x playlist).
    fit = dict(offset=1000.0, slope=1.00001)
    to_playlist = lambda r: int((r - fit["offset"]) / fit["slope"])
    clips = [dict(category="silence", start_sample=4000, end_sample=to_playlist(bursts[0][0]) - 8000)]
    clips += [dict(category="talk", start_sample=to_playlist(a), end_sample=to_playlist(b), reference="x") for a, b in bursts[:2]]
    clips += [dict(category="noise", start_sample=to_playlist(bursts[2][0]) + SR, end_sample=to_playlist(bursts[2][0]) + 3 * SR)]
    clips += [dict(category="quiet", start_sample=to_playlist(bursts[3][1]) + 2 * SR, end_sample=to_playlist(bursts[3][1]) + 3 * SR, reference="y")]
    clips += [dict(category="talk", start_sample=10 ** 9, end_sample=10 ** 9 + SR, reference="after the played part")]
    for i, c in enumerate(clips):
        c["index"] = i
    (tmp / "manifest.json").write_text(json.dumps(dict(clips=clips)))
    score = dict(raw=str(raw), manifest=str(tmp / "manifest.json"), played_seconds=len(pcm) / SR, alignment=fit, arms={})
    (tmp / "score.json").write_text(json.dumps(score))
    (tmp / "base.json").write_text(json.dumps(base))
    out = tmp / "cov.json"
    text = run(sys.executable, tool / "coverage.py", "--score", tmp / "score.json", tmp / "base.json", "--replay", binary,
               "--arm", "deaf=--set adaptive_speech_floor=5000", "--json", out).stdout
    cov = json.loads(out.read_text())["arms"]
    b, deaf = cov["base"], cov["deaf"]
    assert b["speech"]["total"]["clips"] == 3 and b["speech"]["categories"]["talk"]["outside_s"] == 0
    assert abs(b["speech"]["categories"]["quiet"]["outside_s"] - 1.0) < 1e-9 and b["speech"]["total"]["no_segment"] == 1
    assert b["nonspeech"]["silence"]["inside_s"] == 0 and abs(b["nonspeech"]["noise"]["inside_s"] - 2.0) < 1e-3
    assert b["load"]["segments"] == len(segs) and b["load"]["hard_limit_cuts"] == base["hard_limit_cuts"]
    assert b["load"]["segments_per_minute"] == round(len(segs) / (len(pcm) / SR / 60), 2)
    total = sum((c["end_sample"] - c["start_sample"]) / SR for c in clips[:-1] if c["category"] in ("talk", "quiet"))
    assert deaf["load"]["segments"] == 0 and abs(deaf["speech"]["total"]["outside_s"] - total) < 1e-9
    assert "deaf - base" in text and "speech outside segments (s)" in text

    # The same mapping as tools/sttraw_ab/score.py (functions taken from its source; importing the
    # module would load the recognizer's files).
    source = (root / "tools/sttraw_ab/score.py").read_text()
    wanted = {"make_to_src", "assign"}
    picked = [n for n in ast.parse(source).body
              if (isinstance(n, ast.FunctionDef) and n.name in wanted) or
              (isinstance(n, ast.Assign) and getattr(n.targets[0], "id", "") in ("SR", "COLLAR", "SPEECH_MIN", "NO_SPEECH"))]
    assert {n.name for n in picked if isinstance(n, ast.FunctionDef)} == wanted
    ns = {}
    exec(compile(ast.Module(body=picked, type_ignores=[]), "score.py", "exec"), ns)
    played = [c for c in clips if c["end_sample"] <= int(score["played_seconds"] * SR)]
    owners = ns["assign"](segs, played, ns["make_to_src"](fit))
    for c in played:
        assert b["clips"][str(c["index"])]["segments"] == sum(o is c for o in owners), c
    assert b["nonspeech"]["unassigned_segments"] == sum(o is None for o in owners)

    # --check against a stored arm: passes on the same numbers, fails on different ones.
    stored = dict(segments=len(segs), categories={k: dict(ref_s=v["ref_s"], outside_s=v["outside_s"]) for k, v in b["speech"]["categories"].items()},
                  total=dict(outside_s=b["speech"]["total"]["outside_s"]))
    score["arms"] = dict(production=stored, other=dict(stored, segments=len(segs) + 1))
    (tmp / "score.json").write_text(json.dumps(score))
    run(sys.executable, tool / "coverage.py", "--score", tmp / "score.json", tmp / "base.json", "--check", "production")
    assert run(sys.executable, tool / "coverage.py", "--score", tmp / "score.json", tmp / "base.json", "--check", "other", ok=False).returncode == 1

    # 5. Proxy maker (needs numpy): same length, the speech level lands on the target.
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("numpy missing: make_proxy.py not exercised")
    else:
        made = json.loads(run(sys.executable, tool / "make_proxy.py", "--score", tmp / "score.json", "--target-p90", 44,
                              "--out", tmp / "proxy" / "raw_proxy.pcm").stdout)
        assert made["samples"] == len(pcm) and abs(made["after"]["speech_p90"] - 44) <= 1.5, made
        assert (tmp / "proxy" / "raw_proxy.pcm").stat().st_size == 2 * len(pcm)
        quiet = json.loads(run(binary, tmp / "proxy" / "raw_proxy.pcm", "--sidecar", tmp / "proxy" / "raw_proxy.json").stdout)
        assert quiet["samples"] == len(pcm)
print("segmenter replay checks passed")
