# Bug Fix Evidence

Screenshots captured via MCP emulator tools proving that reported bugs are fixed.
Each subdirectory corresponds to a fix commit and contains before/after screenshots
or proof-of-fix screenshots from a two-instance headless test.

## How screenshots are captured

During MCP-based testing, call:
```
screenshot(instance_id='p1', save_path='test/evidence/<fix-dir>/p1_after.png')
```

The `save_path` parameter (added to `tools/mcp_gamestate/server.py`) saves the
PNG alongside returning it to the conversation, so evidence can be committed with
the fix.

## Directory structure

```
test/evidence/
  <fix-slug>/
    p1_before.png   # optional: what it looked like broken
    p1_after.png    # proof the fix works on P1's screen
    p2_after.png    # proof the fix works on P2's screen
    notes.txt       # brief description of what each screenshot shows
```
