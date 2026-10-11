# Super Mario: Worlds United

A playable crossover remix fusing **Super Mario World (SNES)**, **Super Mario Land 2 (GB)**,
and **Super Mario 64 (N64)** moves — written in pure Rust with zero dependencies.
All art is original hand-drawn pixel art for this remix. All text is English only.

## Run it

```sh
cargo run --release
```

No install needed beyond Rust. Renders in your terminal with true-color.

### Controls

| Key | Action |
|-----|--------|
| A / D or arrows | Move left / right |
| Space / X / K | Jump (variable height, hold = higher) |
| Z / J (hold) | Run button (B) |
| S + ... | Duck / slide |
| Down in air | Ground pound (SM64) |
| Z-tap in air at speed | Dive (SM64) |
| Jump while sliding | Long jump (SM64) |
| Down+Jump while slow | Backflip (SM64) |
| Jump into a wall, then Jump | Wall kick (SM64) |
| Jump 3x in rhythm at speed | Triple jump (SM64) |
| Enter | Start / confirm |
| Q / Esc | Quit |

## The game

13 levels across all 6 SML2 zones plus Wario's Castle finale:

- **Tree Zone** (Hillside Hop, Tree Tops) — SMW-style grass + SML2 tree platforms
- **Space Zone** (Moon Launch, Starry Vault) — low gravity, star coins
- **Macro Zone** (Giant Garden, Macro Works) — giant blocks, gears
- **Pumpkin Zone** (Creepy Hollow, Witch Way) — ghosts, spikes
- **Mario Zone** (Toybox Trouble, Clockwork Keep) — toys, clockwork
- **Turtle Zone** (Reef Ride, Turtle Rock) — water, shells
- **Wario's Castle** — boss fight (Wario, fireballs + stomps)

SMW systems: run meter, spin-adjacent slide, Q-blocks with mushrooms/fire flowers,
fireballs, dragon coins (5/level), checkpoints, goal tape, score + lives + timer.

SML2 systems: all 6 zones with themed coins, Moon/Star low-grav physics,
turtle-shell helmets, Wario boss, castle gate progression.

SM64 systems: triple jump / long jump / backflip / wall kick / ground pound / dive,
8 red coins per level awarding a star, 5 dragon coins, coin/star economy on HUD.

## Verify

```sh
cargo test        # 19 tests: physics, enemies, pound, triple jump, bot clears every level
./target/release/worlds-united --bot    # headless bot clears all 12 normal levels
./target/release/worlds-united --shots shots/  # PNG screenshot per level
```

## Notes

- Original code + art; no ROM data used or needed.
- All-English; no Japanese text anywhere (checked).
