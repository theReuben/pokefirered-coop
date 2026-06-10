# Engineering Discipline

This file is loaded into every agent session via CLAUDE.md. It exists because
of a measurable failure pattern: between 2026-06-05 and 2026-06-09, the lab
rival fight and starter selection were "fixed" eleven times across commits
`aa0f9aa4..352f1341`, with the same root causes resurfacing under new
symptoms each time. Every rule below is a countermeasure to a mistake that
actually shipped. When a rule feels like overhead, `git show` the cited
commit and decide whether you are about to repeat it.

These rules carry the same weight as the DO NOTs in CLAUDE.md.

---

## 1. Diagnose the mechanism, not the symptom

A bug is understood when you can state the complete cause→effect chain and
point to the code implementing each link. "I changed X and the symptom went
away" is not a diagnosis — with this codebase's redundant recovery layers, a
wrong fix frequently makes the symptom vanish by coincidence.

- Canonical example: `352f1341` "fixed" the missing Bulbasaur ball by
  clearing hide-flags at scene 2, diagnosing "stale flags from a mid-session
  save". The actual mechanism (found later, fixed in `2fa7139f`) was the
  starter auto-recovery fabricating a phantom Bulbasaur pick from
  `VAR_STARTER_MON`'s default 0 and broadcasting it. The wrong fix passed
  verification because the test fixtures happened to make P1 the Bulbasaur
  picker.
- Write down, in the commit message, what you **observed** (logs, memory
  reads, repro steps) separately from what you **inferred**. `f83d21bb` had
  to retract an earlier session's misreading of
  `sGlobalScriptContextStatus=2` as "script running" — a guess that had been
  recorded as fact and steered a later session wrong.
- Corollary: commit messages and code comments from earlier sessions are
  hypotheses, not ground truth. They have been wrong before. Verify against
  the code before building on them.

## 2. The second-occurrence rule

Before writing a fix, grep for the same fix pattern elsewhere in the tree.
If it already exists once, you are looking at a layer bug wearing a local
disguise — fix the layer, not the call site.

- The "add `Multiplayer_Update()` at the top of the poll" patch was applied
  three times (`ed7b9385`, then twice in `69822bae`). The second commit even
  says "same root cause as NativePollPartySync" — the recurrence was
  *recognized* and patched locally anyway. The real fix (`6b7dcb1d`) was one
  hook in the script engine that fixed all current and future wait commands.
- "Keep changes minimal" in CLAUDE.md means the smallest **correct** change,
  not the smallest diff. A one-line patch repeated at N call sites is a
  larger, worse change than one structural fix. Touching shared engine code
  (script.c, battle_setup.c) is in scope when the bug lives there.

## 3. State placement: pick the tier before writing the field

| Kind of fact | Lives in | Examples |
|---|---|---|
| Durable world state — survives save/reload/reconnect | Saved game vars/flags (`VAR_*`, whitelisted flags) | `VAR_RIVAL_STARTER`, `VAR_PARTNER_STARTER`, trainer-defeated flags |
| Session state — valid only while connected | `gMultiplayerState` (IWRAM) | connState, ghost slot, readiness, turn buffers |
| Per-map scratch | `VAR_TEMP_*` / `FLAG_TEMP_*` | trigger columns, scene-local cursors |

- IWRAM fields are zeroed by `Multiplayer_Init` and save-state reloads.
  Anything permanent cached there must be **recoverable from the durable
  tier** — and the recovery must distinguish "unset" from a legitimate zero
  value (`VAR_STARTER_MON == 0` is also "slot 0 = Bulbasaur"; that ambiguity
  was the phantom-pick bug. Gate on a flag like `FLAG_SYS_POKEMON_GET`).
- Setting a **persistent** flag from a **network event** is a red flag:
  partner state leaked into save data caused the recurring ball-visibility
  bugs. Prefer deriving: store the durable fact once, recompute presentation
  state from it on entry (`Multiplayer_RederiveStarterBallFlags`).
- `VAR_TEMP_*`/`FLAG_TEMP_*` have exactly one owner per map. Declare every
  alias in the `.equ` block at the top of the map's scripts.inc; never reuse
  one for a second purpose (the `VAR_TEMP_2` double-booking caused the rival
  battle desync, `83d35464`). C code must never read a temp var — trigger
  scripts overwrite them freely.

## 4. Protocol changes touch every layer — use the checklist

The transport stack has four independent copies of packet knowledge. The
Tauri bridge's tables silently stopped at 0x0F while the ROM grew to 0x1A;
every newer packet **corrupted the stream framing** on the real relay path
for weeks (`4fba00a5`), invisible to MCP testing because the in-process
relay copies bytes without framing.

Adding or changing a packet type requires, in one commit:

1. `include/constants/multiplayer.h` — type id + `MP_PKT_SIZE_*`
2. `src/multiplayer.c` — encoder/handler, plus a native test
3. `tauri-app/src-tauri/src/serial_bridge.rs` — **all three** tables:
   `packet_size`, `packet_to_json`, `json_to_packet`, plus an in-crate test
4. `relay-server/src/server.ts` — message union + forwarding case, plus a
   vitest case (opaque peer-to-peer payloads can ride the generic
   `{type:"raw", bytes:hex}` passthrough)
5. `tools/mcp_gamestate/server.py` — `_PKT_SIZES` chaos framing table

If you cannot update a layer (e.g. no Rust toolchain), say so explicitly in
the commit message instead of leaving it silently inconsistent.

Reliability mechanics live in the transport, not per message: if a new
exchange needs retry/recovery, extend the `MP_PKT_STATE_BEACON` payload —
do not add another resend timer. Two band-aids that happen to cancel out
(receive-side validation silently eating garbage that a resend loop then
papered over) are how `Multiplayer_SendStarterPick`'s wrong var read
survived "verified" for days.

## 5. Verification standards

- **`make check-native` passes before every commit. No exceptions.** If it
  does not even compile, fixing it is your first task — it was uncompilable
  for six commits (broken by `69822bae`, repaired in `06ad530c`) while
  "verified" fixes landed on a red CI, and five stale assertions never ran
  against the features they covered.
- **check-native is a host build; it cannot see GBA constraints.** The
  native suite compiles multiplayer.c with host gcc, which happily accepts
  things the ARM target rejects — a nonzero-initialized static landed in
  `.data` (a section the GBA link script discards) and broke every ROM
  build while check-native stayed green (`b2a48b95`). In ROM-side code:
  statics must be zero-initialized or `const`; use `EWRAM_DATA` for
  NULL-initialized pointers. If you cannot run `make firered` locally,
  state that in the commit and treat CI's ROM build as the gate.
- **Fixed-role fixtures prove less than they appear to.** The save-state
  catalogue hard-codes P1 at the Bulbasaur ball and P2 at Charmander; that
  symmetry masked the phantom-pick bug completely. For any
  selection/role-dependent feature, run at least one pass with the roles or
  choices swapped from the fixture default.
- **Sync features must survive loss and reordering.** The MCP relay is
  lossless and ordered; the real relay is not. Re-run the scenario with
  `set_link_chaos(drop=0.3, seed=1)` and confirm convergence before calling
  a sync feature done.
- **Trial-and-error belongs in the working tree, not the history.** Three
  successive commits (`9a206c7c` → `aa75b9e2` → `b77ba2a1`) shipped guesses
  at one step-back animation. Iterate in the emulator until the behavior is
  understood, then commit once. If you must checkpoint mid-investigation,
  label the commit as an experiment, not a fix.
- **"Verified" must name what ran.** List the commands, scenarios, and
  states used. A bare "verified" in a commit message becomes false
  institutional memory the moment an untested path breaks.

## 6. The spec is binding until amended

If CLAUDE.md specifies a mechanism, implement that mechanism — or amend the
spec in the same commit with your rationale. The spec called for the rival's
starter to be stored in a saved `VAR_RIVAL_STARTER` from the start; it was
silently skipped in favor of IWRAM fields, and that single omission produced
the entire rival-desync bug family. Silent divergence is the most expensive
failure mode in this repo, because the next session plans against the spec.

## 7. End-of-session honesty

Long sessions degrade. The six-fix churn night of 06-08/06-09 ran past
2 AM with each fix narrower than the last. If you notice you are patching
your own patch from earlier in the same session, stop: re-derive the
mechanism (rule 1) or write up the open state honestly in PROGRESS.md and
end the session. An accurate "this is not fixed yet, here is what I know"
is worth more than a plausible fix that costs the next session a day.
