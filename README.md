# pokefirered-coop

Networked 2-player co-op mod for Pokémon FireRed, built on pokeemerald-expansion.

## Building

Requires devkitARM. See [INSTALL.md](INSTALL.md) for toolchain setup.

```bash
make firered -j4
```

Output: `pokefirered.gba` (32 MB). One expected linker warning about RWX ELF segments is normal.

## Project Plan

See [PROGRESS.md](PROGRESS.md) for implementation phases and current status.
See [CLAUDE.md](CLAUDE.md) for architecture and implementation guidelines.