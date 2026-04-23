# Architecture Decision Records

Claude: check this file before making significant technical decisions.
Do NOT contradict existing ADRs without explicitly superseding them.

Format:
## ADR-NNN: Title
**Date:** YYYY-MM-DD | **Status:** accepted
**Context:** Why | **Decision:** What | **Consequences:** Impact

---

## ADR-001: Save File Strategy and Session Identity
**Date:** 2026-04-23 | **Status:** accepted

**Context:** Each player runs their own ROM instance with their own GBA `.sav` file storing personal data (party, bag, PC, money, Pokédex, badges). Shared world state (trainer/story/item flags) is synced at runtime via the flag sync layer. Two problems arise: (1) the encounter seed is not stored in the GBA save, so reconnecting players would get a different randomisation each session; (2) there is nothing stopping a player from accidentally pairing the wrong save with a partner, causing flag divergence or mismatched encounter tables.

**Decision:**
- Each player owns their own `.sav` file. Personal data (party, bag, PC) is never synced — each player's ROM handles it natively.
- A small sidecar file (`.coop`, JSON) lives alongside the `.sav` and stores session metadata:
  ```json
  {
    "session_id": "<UUID v4>",
    "encounter_seed": <u32>,
    "created_at": "YYYY-MM-DD"
  }
  ```
- On **first session**: the host generates both the `session_id` and `encounter_seed`, broadcasts them during the relay handshake, and both apps write their sidecar files.
- On **subsequent sessions**: the host broadcasts their `session_id`; the guest app checks it matches their loaded sidecar. A mismatch is rejected with a user-facing error before gameplay begins.
- The Tauri load screen displays sidecar metadata (date created, hours played) so players can identify the correct save without guessing.
- **Lost sidecar recovery**: if a player loses their `.coop` file they can request their partner to share theirs. The app should support importing a partner's sidecar to recreate a matching one (since the session ID and seed are all that's needed).
- **Flag reconciliation on connect**: union-wins — any flag set in either player's save is applied to both on connect, regardless of when it was set. This handles offline solo play between sessions.
- **Solo offline play**: permitted. Flag divergence from solo play is resolved by union-wins on next connect. The session ID does not prevent solo play; it only ensures the correct two saves are paired.

**Consequences:**
- Encounter tables are stable across sessions for the same co-op run — no re-randomisation on reconnect.
- Pairing the wrong saves is caught at handshake time with a clear error, before any state is exchanged.
- Players have a familiar "choose your save file" UX in the Tauri app, consistent with standard emulator behaviour.
- Lost sidecar is a recoverable edge case, not a hard failure, as long as the partner still has theirs.
- Synced double battle (Phase 8) is unaffected — party sync happens live at battle start, not from save files.

---
