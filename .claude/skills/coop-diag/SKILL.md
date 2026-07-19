---
name: coop-diag
description: Run scripted two-instance co-op sync diagnostics (ghost sync, field-trainer lock, rival coop battle) from docs/TEST_PLAYBOOK.md, optionally delegating execution to cheap haiku subagents, then analyze the reports for real bugs vs harness artifacts. Use when asked to test/verify co-op sync, run diagnostics, or generate test data.
---

# /coop-diag [scenario...] [--chaos] [--haiku]

Scenarios: `R1` (Route 1 ghost position sync), `F1` (field-trainer lock,
run BOTH battler roles), `RB1` (co-op rival double battle entry). Default:
all three, chaos variants included.

## Procedure

1. **Preflight (do this yourself, never skip):**
   - `git log --oneline -3` and check `pokefirered.gba` mtime is NEWER than
     the latest src commit. If stale: rebuild ROM, regen memory map, and
     `make build-states` first (see the windows-build-toolchain memory /
     CLAUDE.md build notes). Testing a stale ROM produces false results and
     has burned multiple sessions.
   - Confirm the five native suites pass (see CLAUDE.md check-native notes).
2. **Execution:** read `docs/TEST_PLAYBOOK.md`. With `--haiku`, spawn ONE
   general-purpose haiku subagent per scenario, sequentially (the two
   emulator instances are shared global state — never run two scenario
   agents concurrently). The subagent prompt: "Read docs/TEST_PLAYBOOK.md,
   execute Scenario <X> (and its chaos variant), write reports per its
   report format, stop both emulators, reply with a 5-line summary."
   Without `--haiku`, drive the MCP tools yourself.
3. **Analysis (always yourself, never the subagent's verdict):** read each
   `test/reports/*.md`. For every FAIL decide: ROM bug / harness artifact /
   playbook error. Known harness artifacts to rule out first:
   - Long `wait` on one instance freezes the other → relay declares it
     silent → ghost despawn. Only interleaved waits test the heartbeat.
   - Transient empty `read_memory`/`get_text_state` reads (retry).
   - Recordings run on wall clock, not emulated frames.
   - Same-state-for-both-instances loads (trainer ID collision).
4. **Desync red flags** (from CLAUDE.md): if `check_battle_sync()` FAILs or
   one side is in battle while the other is on the overworld, treat as
   critical, collect both `battle_diag()` outputs, and stop that scenario.
5. **Verdict:** summarize per scenario: PASS / bug (with mechanism
   hypothesis + evidence paths) / needs-rerun. File real bugs in
   PROGRESS.md. Do not fix code as part of this skill.
