# P4 bench tools

Board-side bench scripts for the P4 speech work. The ones that talk to a board over the console
(`bench.py`, `continuous_bench.py`, `g2_user_connect.py`, `s3_keep_web.py`, `transcript_*.py`,
`transcription_*.py`) need two settings:

- `HW1_BENCH_LIBS`: a folder holding HardwareOne's former bench helpers `p4_ble_roles/`,
  `p4_connectivity/` and `p4_mesh/`. HardwareOne removed them from its repository on 2026-11-26;
  they are kept privately, not in either repository.
- `HW1_CREDENTIALS`: the device credentials JSON (never committed).

Logs and results go to the git-ignored `tools/stt_p4/private/`.
