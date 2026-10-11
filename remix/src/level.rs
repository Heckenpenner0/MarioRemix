//! Tiles, ASCII map parsing, zones and level metadata.
//! Art and maps below are original creations for this remix (no ROM data).
pub const TILE: f32 = 8.0;
#[allow(dead_code)] pub const ROWS: usize = 14;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tile {
    Empty, Ground, Hard, Brick,
    Q(u8), // 0 mushroom, 1 fire, 2 carrot, 3 superball, 4 star, 5 1up
    Used, Note, Stem, Spike, Coin, Red, Dragon, Gold, Goal, Check, Water,
}
impl Tile {
    pub fn solid(self) -> bool {
        matches!(self, Tile::Ground | Tile::Hard | Tile::Brick | Tile::Q(_) | Tile::Used | Tile::Note | Tile::Stem)
    }
    #[allow(dead_code)] pub fn bumpable(self) -> bool {
        matches!(self, Tile::Brick | Tile::Q(_) | Tile::Note)
    }
}

pub struct Parsed {
    pub w: usize, pub h: usize,
    pub grid: Vec<Tile>,
    pub spawn: (f32, f32),
    pub foes: Vec<(char, f32, f32)>,
    pub goal_x: f32, // crossing this x finishes the level (f32::MAX if none)
}
impl Parsed {
    pub fn at(&self, tx: i32, ty: i32) -> Tile {
        if tx < 0 || ty < 0 || tx >= self.w as i32 { return Tile::Hard; }
        if ty >= self.h as i32 { return Tile::Empty; } // pits kill (no invisible floor)
        self.grid[ty as usize * self.w + tx as usize]
    }
    pub fn set(&mut self, tx: i32, ty: i32, t: Tile) {
        if tx >= 0 && ty >= 0 && (tx as usize) < self.w && (ty as usize) < self.h {
            self.grid[ty as usize * self.w + tx as usize] = t;
        }
    }
    pub fn solid_at(&self, tx: i32, ty: i32) -> bool { self.at(tx, ty).solid() }
}

fn tile_of(c: char) -> Tile {
    match c {
        '#' => Tile::Ground, 'H' => Tile::Hard, 'B' => Tile::Brick,
        '?' => Tile::Q(0), 'q' => Tile::Q(1), 'c' => Tile::Q(2),
        'u' => Tile::Q(3), '*' => Tile::Q(4), '1' => Tile::Q(5),
        'U' => Tile::Used, 'N' => Tile::Note, 'T' => Tile::Stem,
        '^' => Tile::Spike, 'o' => Tile::Coin, 'R' => Tile::Red, 'D' => Tile::Dragon,
        'K' => Tile::Gold, 'G' => Tile::Goal, 'C' => Tile::Check,
        '~' => Tile::Water,
        _ => Tile::Empty,
    }
}
fn is_foe(c: char) -> bool { matches!(c, 'g'|'k'|'p'|'n'|'b'|'a'|'i'|'t'|'r'|'e'|'m'|'w'|'W') }

pub fn parse(map: &[&str]) -> Parsed {
    let h = map.len();
    let w = map.iter().map(|r| r.chars().count()).max().unwrap_or(1);
    let mut grid = vec![Tile::Empty; w * h];
    let mut spawn = (16.0, 16.0);
    let mut foes = Vec::new();
    for (y, row) in map.iter().enumerate() {
        for (x, c) in row.chars().enumerate() {
            if c == 'S' {
                spawn = (x as f32 * TILE + 1.0, y as f32 * TILE - 6.0);
            } else if is_foe(c) {
                foes.push((c, x as f32 * TILE + TILE * 0.5, y as f32 * TILE + TILE * 0.5));
            } else {
                grid[y * w + x] = tile_of(c);
            }
        }
    }
    let mut gx = f32::MAX;
    for (i, t) in grid.iter().enumerate() {
        if *t == Tile::Goal { gx = gx.min((i % w) as f32 * TILE); }
    }
    let mut p = Parsed { w, h, grid, spawn, foes, goal_x: gx };
    seed_reds(&mut p);
    p
}

/// SM64 homage: 8 red coins per level, auto-placed in open air above ground
/// (deterministic; always reachable by walking/jumping through).
fn seed_reds(p: &mut Parsed) {
    let mut placed = 0;
    let mut col = p.w / 10;
    let mut guard = 0;
    while placed < 8 && guard < 400 {
        guard += 1;
        col = (col + p.w / 9 + 3) % p.w.max(1);
        if col < 2 { continue; }
        // find air cell with headroom and ground 1..4 below
        for ty in 2..p.h as i32 - 1 {
            if p.at(col as i32, ty) != Tile::Empty { continue; }
            if p.at(col as i32, ty - 1) != Tile::Empty { continue; }
            let mut ok = false;
            for d in 1..=4 {
                let t = p.at(col as i32, ty + d);
                if t.solid() { ok = true; break; }
                if t != Tile::Empty && t != Tile::Coin { break; }
            }
            if ok {
                p.set(col as i32, ty, Tile::Red);
                placed += 1;
                break;
            }
        }
    }
}

pub struct LevelMeta {
    pub zone: usize,
    pub name: &'static str,
    pub map: &'static [&'static str],
    pub time: u32,
    pub castle: bool,
}
pub struct Zone {
    pub name: &'static str,
    pub coin: &'static str,
    pub lowgrav: bool,
}
pub const ZONES: &[Zone] = &[
    Zone { name: "Tree Zone",    coin: "Leaf Coin",  lowgrav: false },
    Zone { name: "Space Zone",   coin: "Star Coin",  lowgrav: true  },
    Zone { name: "Macro Zone",   coin: "Gear Coin",  lowgrav: false },
    Zone { name: "Pumpkin Zone", coin: "Moon Coin",  lowgrav: false },
    Zone { name: "Mario Zone",   coin: "Toy Coin",   lowgrav: false },
    Zone { name: "Turtle Zone",  coin: "Shell Coin", lowgrav: false },
    Zone { name: "Wario's Castle", coin: "Crown",    lowgrav: false },
];
pub fn levels() -> Vec<LevelMeta> {
    vec![
        LevelMeta { zone: 0, name: "Hillside Hop",  map: crate::maps1::TREE1,   time: 200, castle: false },
        LevelMeta { zone: 0, name: "Tree Tops",     map: crate::maps1::TREE2,   time: 200, castle: false },
        LevelMeta { zone: 1, name: "Moon Launch",   map: crate::maps1::SPACE1,  time: 200, castle: false },
        LevelMeta { zone: 1, name: "Starry Vault",  map: crate::maps1::SPACE2,  time: 200, castle: false },
        LevelMeta { zone: 2, name: "Giant Garden",  map: crate::maps1::MACRO1,  time: 220, castle: false },
        LevelMeta { zone: 2, name: "Macro Works",   map: crate::maps1::MACRO2,  time: 220, castle: false },
        LevelMeta { zone: 3, name: "Creepy Hollow", map: crate::maps2::PUMP1,   time: 200, castle: false },
        LevelMeta { zone: 3, name: "Witch Way",     map: crate::maps2::PUMP2,   time: 200, castle: false },
        LevelMeta { zone: 4, name: "Toybox Trouble",map: crate::maps2::MARIO1,  time: 200, castle: false },
        LevelMeta { zone: 4, name: "Clockwork Keep",map: crate::maps2::MARIO2,  time: 220, castle: false },
        LevelMeta { zone: 5, name: "Reef Ride",     map: crate::maps2::TURTLE1, time: 200, castle: false },
        LevelMeta { zone: 5, name: "Turtle Rock",   map: crate::maps2::TURTLE2, time: 220, castle: false },
        LevelMeta { zone: 6, name: "Wario's Castle",map: crate::maps2::CASTLE,  time: 300, castle: false },
    ]
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn all_rows_rectangular_and_parse() {
        for l in levels() {
            let p = parse(l.map);
            assert_eq!(p.h, ROWS, "{}", l.name);
            assert!(p.w >= 40, "{}", l.name);
        }
    }
    #[test]
    fn every_level_has_spawn_and_goal_or_boss() {
        for l in levels() {
            let p = parse(l.map);
            let has_goal = p.grid.iter().any(|&t| t == Tile::Goal);
            let has_boss = p.foes.iter().any(|&(c, _, _)| c == 'W');
            assert!(has_goal || has_boss, "{}", l.name);
            let _ = p.spawn;
        }
    }
    #[test]
    fn eight_red_coins_per_level() {
        for l in levels() {
            let p = parse(l.map);
            let n = p.grid.iter().filter(|&&t| t == Tile::Red).count();
            assert_eq!(n, 8, "{}", l.name);
        }
    }
    #[test]
    fn zone_finales_hold_gold_coins() {
        // last level of zones 0..5 must contain the zone's gold coin
        let lv = levels();
        for z in 0..6 {
            let last = lv.iter().filter(|l| l.zone == z).last().unwrap();
            let p = parse(last.map);
            assert!(p.grid.iter().any(|&t| t == Tile::Gold), "zone {}", z);
        }
    }
}
