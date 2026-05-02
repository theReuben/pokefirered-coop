# Pokémon FireRed Co-Op — Player Guide

Two players. One Kanto. Play through Pokémon FireRed together over the internet.

---

## What you need

- Two computers (macOS, Windows, or Linux)
- Both players download the same version of the app
- An internet connection on both machines

---

## Download & Install

Go to the [Releases](../../releases) page and download the installer for your OS:

| OS | File to download |
|---|---|
| macOS | `Pokémon FireRed Co-Op_*.dmg` |
| Windows | `Pokémon FireRed Co-Op_*-setup.exe` |
| Linux (Debian/Ubuntu) | `pokefirered-coop_*.deb` |

**macOS:** Open the `.dmg` and drag the app to your Applications folder. The first time you open it, right-click the app icon and choose **Open** (macOS may block unsigned apps; this bypasses the warning).

**Windows:** Run the installer. Windows Defender may show a SmartScreen warning — click **More info → Run anyway**.

**Linux:** Install with `sudo dpkg -i pokefirered-coop_*.deb`, then launch from your application menu.

---

## Starting a new co-op game

### Player 1: Host

1. Open the app and click **Host Game**.
2. Click **New Game**.
3. Choose where to save your game file (a `.sav` file — pick any folder).
4. Decide whether to enable **Randomize wild Pokémon** (on by default — both players see the same randomized encounters).
5. Your **room code** appears on screen (e.g. `TR4N9K`). Share this with Player 2.
6. The game starts automatically when Player 2 connects.

### Player 2: Join

1. Open the app and click **Join Game**.
2. Type the 6-character room code from Player 1.
3. Choose where to save your game file (a fresh `.sav` file).
4. Click **Connect** — the game starts when the connection is confirmed.

Both players now see the intro and begin on their own screen. You share the same game world: trainers you defeat stay defeated for both of you.

---

## Controls (keyboard)

| Key | GBA Button |
|---|---|
| Arrow keys | D-Pad |
| Z | A |
| X | B |
| A | L |
| S | R |
| Enter | Start |
| Backspace | Select |

---

## Choosing your starter

In Oak's Lab, three Poké Balls are available. When you select a starter:

- Your choice is locked immediately and your partner's app grays out that ball.
- The rival automatically receives whichever starter neither player chose.

If your partner hasn't connected yet when you reach Oak's Lab, all three balls remain available — your partner's pick will be shown as unavailable once they connect.

---

## Gym leaders

When either player approaches a gym leader:

1. The game shows **"Waiting for partner…"** instead of starting the battle immediately.
2. Both players must interact with the same gym leader before the battle begins.
3. Once both are ready, the battle starts on both screens simultaneously.
4. The gym leader's defeat flag is shared — only one player needs to win for it to count for both.

---

## Continuing a saved game

Your session uses two files:

- `pokefirered.sav` — your Pokémon, items, and progress
- `pokefirered.coop` — session ID that links your save to your partner's

**Both players must use the same session to reconnect.**

### Host (resuming)
1. Click **Host Game → Load Save**.
2. Open your `.sav` file. The app reads the `.coop` sidecar automatically and shows the original session date so you can confirm it's the right one.
3. Share the new room code with your partner.

### Guest (resuming)
1. Click **Join Game**.
2. Enter the new room code.
3. Open your `.sav` file when prompted.

> **Keep your `.sav` and `.coop` files in the same folder.** The app looks for the `.coop` sidecar next to the `.sav` file by name. If the `.coop` file is missing, the session cannot be resumed.

---

## Randomized wild Pokémon

When randomization is on (default):

- Wild encounters are shuffled across all 151–493 Pokémon.
- Both players see the same encounter tables (the seed is shared at session start).
- Level ranges from the original routes are kept — only species change.
- The setting is chosen by the host and cannot be changed mid-session.

---

## Known limitations

| Limitation | Notes |
|---|---|
| No gamepad support | Keyboard only for now |
| Boss battles | Both players fight the gym leader independently; no shared double battle yet |
| No in-game chat | Use Discord, voice chat, or any external app to communicate |
| Ghost NPC (partner's on-screen position) | Visible when both players are on the same map; not shown across maps |
| No trading or co-op battles vs wild Pokémon | Players manage their own parties separately |

---

## Troubleshooting

**"Session mismatch" error when joining**
The room code is correct but the session IDs don't match. The host needs to use **Load Save** (not New Game) so the original session ID is preserved.

**"Room full" error**
A third player tried to connect. Each session supports exactly 2 players. Start a fresh session if you need to swap players.

**Partner's position not updating**
Check that both players are on the same map. The ghost NPC only appears when you are on the same map. If the issue persists, try walking to a different map and back.

**Game is slow or laggy**
The relay server is in the US. If both players are outside the US, latency may be higher. Position updates happen every ~4 frames; the game continues normally even if some packets are delayed.

**App won't open on macOS ('"Pokémon FireRed Co-Op" cannot be opened because the developer cannot be verified')**
Right-click the app icon in Finder → **Open** → **Open** again in the dialog. You only need to do this once.

**Save file not writing on exit**
The app saves automatically when you close it. If the app crashes, the save may not have written. Use the in-game **Save** option (Start menu → Save) regularly to keep a backup.

---

## File reference

| File | What it is | Keep it? |
|---|---|---|
| `pokefirered.sav` | Your Pokémon and game progress | Yes — back it up |
| `pokefirered.coop` | Session ID (links to partner's session) | Yes — keep next to `.sav` |

---

*Pokémon © Nintendo / Game Freak. This mod is a fan project, not affiliated with or endorsed by Nintendo. Free for personal use.*
