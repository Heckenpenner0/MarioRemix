//! Enemy roster fusing SMW and SML2 bestiaries. Deterministic, no RNG.
use crate::level::TILE;
#[allow(unused_imports)] use crate::level::Tile as _Tile;
use crate::physics::{Body, move_body};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FoeKind {
    Goomba, Koopa, Shell, Piranha, Spiny, Bat, Ant, Pig,
    Octopus, Bird, Fish, Trooper, Thwomp, Wario,
}

pub struct Foe {
    pub kind: FoeKind,
    pub b: Body,
    pub dir: f32,
    pub t: u32,
    pub ax: f32, pub ay: f32,
    pub hp: u8,
    pub alive: bool,
    pub ph: u32,   // phase timer
    pub state: u8, // behavior state
    pub active: bool, // wario: fight started
}

pub fn spawn(kind: FoeKind, cx: f32, cy: f32) -> Foe {
    let (w, h, hp) = size_hp(kind);
    let mut f = Foe {
        kind, b: Body::new(cx - w * 0.5, cy - h * 0.5, w, h),
        dir: -1.0, t: 0, ax: cx, ay: cy, hp, alive: true, ph: 0, state: 0, active: false,
    };
    if kind == FoeKind::Wario { f.b.vx = 0.0; }
    f
}
fn size_hp(k: FoeKind) -> (f32, f32, u8) {
    match k {
        FoeKind::Goomba => (10.0, 8.0, 1),
        FoeKind::Koopa => (10.0, 12.0, 1),
        FoeKind::Shell => (10.0, 8.0, 1),
        FoeKind::Piranha => (12.0, 14.0, 1),
        FoeKind::Spiny => (12.0, 8.0, 1),
        FoeKind::Bat => (12.0, 8.0, 1),
        FoeKind::Ant => (12.0, 8.0, 1),
        FoeKind::Pig => (12.0, 10.0, 1),
        FoeKind::Octopus => (10.0, 10.0, 1),
        FoeKind::Bird => (12.0, 8.0, 1),
        FoeKind::Fish => (10.0, 8.0, 1),
        FoeKind::Trooper => (10.0, 14.0, 1),
        FoeKind::Thwomp => (20.0, 20.0, 99),
        FoeKind::Wario => (14.0, 22.0, 3),
    }
}
pub fn foe_of(c: char, cx: f32, cy: f32) -> Option<Foe> {
    let k = match c {
        'g' => FoeKind::Goomba, 'k' => FoeKind::Koopa, 'p' => FoeKind::Piranha,
        'n' => FoeKind::Spiny, 'b' => FoeKind::Bat, 'a' => FoeKind::Ant,
        'i' => FoeKind::Pig, 't' => FoeKind::Octopus, 'r' => FoeKind::Bird,
        'e' => FoeKind::Fish, 'm' => FoeKind::Trooper, 'w' => FoeKind::Thwomp,
        'W' => FoeKind::Wario, _ => return None,
    };
    Some(spawn(k, cx, cy))
}

fn walker(f: &mut Foe, dt: f32, speed: f32, grav: f32, solid: &dyn Fn(i32, i32) -> bool) {
    f.b.vx = f.dir * speed;
    f.b.vy = (f.b.vy + grav * dt).min(220.0);
    move_body(&mut f.b, dt, TILE, solid);
    if f.b.hit_wall != 0 { f.dir = -f.dir; }
    if (f.b.cx() - f.ax).abs() > 44.0 {
        f.dir = (f.ax - f.b.cx()).signum();
        if f.dir == 0.0 { f.dir = 1.0; }
    }
}

pub fn update_foe(f: &mut Foe, dt: f32, solid: &dyn Fn(i32, i32) -> bool,
                  px: f32, py: f32, grav: f32) {
    if !f.alive { return; }
    f.t += 1;
    let dx = px - f.b.cx();
    match f.kind {
        FoeKind::Goomba => walker(f, dt, 22.0, grav, solid),
        FoeKind::Koopa => walker(f, dt, 28.0, grav, solid),
        FoeKind::Spiny => walker(f, dt, 30.0, grav, solid),
        FoeKind::Ant => walker(f, dt, 48.0, grav, solid),
        FoeKind::Trooper => walker(f, dt, 32.0, grav, solid),
        FoeKind::Pig => {
            let charging = dx.abs() < 64.0 && (py - f.b.y).abs() < 24.0 && dx.signum() == f.dir.signum();
            if dx.signum() != 0.0 && dx.signum() != f.dir.signum() && !charging { f.dir = dx.signum(); }
            walker(f, dt, if charging { 85.0 } else { 15.0 }, grav, solid);
        }
        FoeKind::Shell => {
            f.b.vy = (f.b.vy + grav * dt).min(220.0);
            move_body(&mut f.b, dt, TILE, solid);
            if f.b.hit_wall != 0 { f.dir = -f.dir; f.b.vx = f.dir * 140.0; }
        }
        FoeKind::Piranha => {
            // cycle: hidden 110 -> rise 36 -> out 100 -> sink 36; shy when player near
            f.ph += 1;
            let near = dx.abs() < 26.0;
            let cyc = f.ph % 286;
            let out_amt = if cyc < 110 { 0.0 }
                else if cyc < 146 { (cyc - 110) as f32 / 36.0 }
                else if cyc < 246 { 1.0 }
                else { 1.0 - (cyc - 246) as f32 / 40.0 };
            let shy = near && out_amt < 1.0;
            let a = if shy { 0.0 } else { out_amt };
            // anchor: base of plant; body sits atop stem, extends upward by a
            f.b.h = 14.0 * a.max(0.0);
            f.b.y = f.ay - f.b.h;
            f.b.x = f.ax - 6.0;
            if a <= 0.0 { f.b.h = 0.0; }
        }
        FoeKind::Bat => {
            // sine patrol around anchor (no chase): predictable and fair
            let s = (f.t as f32 * 0.06).sin();
            let c = (f.t as f32 * 0.043).cos();
            f.b.x = f.ax - f.b.w * 0.5 + s * 16.0;
            f.b.y = f.ay - f.b.h * 0.5 + s * 8.0 + c * 3.0;
            f.dir = (f.t as f32 * 0.06).cos().signum();
            let _ = (dx, dt);
        }
        FoeKind::Bird => {
            let s = (f.t as f32 * 0.05).sin();
            f.b.x = f.ax - f.b.w * 0.5 + s * 16.0;
            f.b.y = f.ay - f.b.h * 0.5 + (f.t as f32 * 0.09).sin() * 8.0;
            f.dir = (f.t as f32 * 0.05).cos().signum();
        }
        FoeKind::Fish => {
            let s = (f.t as f32 * 0.055).sin();
            f.b.x = f.ax - f.b.w * 0.5 + s * 16.0;
            f.b.y = f.ay - f.b.h * 0.5 + (f.t as f32 * 0.11).sin() * 6.0;
            f.dir = (f.t as f32 * 0.055).cos().signum();
        }
        FoeKind::Octopus => {
            f.b.vy = (f.b.vy + grav * dt).min(220.0);
            f.ph += 1;
            if f.b.on_ground {
                f.b.vx = 0.0;
                if f.ph > 50 {
                    f.ph = 0;
                    f.dir = if (f.b.cx() - f.ax).abs() > 56.0 {
                        (f.ax - f.b.cx()).signum()
                    } else if dx == 0.0 { 1.0 } else { dx.signum() };
                    f.b.vx = f.dir * 42.0;
                    f.b.vy = -185.0;
                }
            }
            move_body(&mut f.b, dt, TILE, solid);
            if f.b.hit_wall != 0 { f.dir = -f.dir; }
        }
        FoeKind::Thwomp => {
            f.ph += 1;
            match f.state {
                0 => { // hover with slight bob; underside stays 3px+ above
                    // a runner's head so the run-through stays fair at any phase
                    f.b.y = f.ay - f.b.h * 0.5 - 2.0 + (f.t as f32 * 0.05).sin() * 1.0;
                    if dx.abs() < 15.0 && py > f.b.y { f.state = 1; f.ph = 0; }
                }
                1 => { // telegraph shake; abort if the player ran past (fair thwomp)
                    f.b.x = f.ax - f.b.w * 0.5 + ((f.ph % 4) as f32 - 1.5);
                    if f.ph > 26 {
                        if (px - f.b.cx()).abs() > 20.0 { f.state = 0; f.ph = 0; }
                        else { f.state = 2; }
                    }
                }
                2 => { // slam
                    f.b.vy = 260.0;
                    move_body(&mut f.b, dt, TILE, solid);
                    if f.b.on_ground { f.state = 3; f.ph = 0; }
                }
                3 => { // rest then rise
                    if f.ph > 60 { f.state = 4; }
                }
                _ => { // rise
                    f.b.y -= 42.0 * dt;
                    if f.b.y <= f.ay - f.b.h * 0.5 { f.b.y = f.ay - f.b.h * 0.5; f.state = 0; }
                }
            }
        }
        FoeKind::Wario => {
            if !f.active {
                if dx.abs() < 150.0 { f.active = true; }
                else { return; }
            }
            f.ph += 1;
            match f.state {
                0 => { // stalk
                    f.dir = if dx == 0.0 { 1.0 } else { dx.signum() };
                    f.b.vx = f.dir * 38.0;
                    f.b.vy = (f.b.vy + grav * dt).min(220.0);
                    move_body(&mut f.b, dt, TILE, solid);
                    if f.b.hit_wall != 0 { f.dir = -f.dir; }
                    if f.ph > 140 && f.b.on_ground {
                        f.state = 1; f.ph = 0; // jump attack
                        f.b.vy = -270.0;
                    } else if f.ph > 220 {
                        f.state = 2; f.ph = 0; // shoulder charge telegraph
                    }
                }
                1 => { // airborne slam toward player
                    f.b.vx = f.dir * 70.0;
                    f.b.vy = (f.b.vy + grav * dt).min(260.0);
                    move_body(&mut f.b, dt, TILE, solid);
                    if f.b.on_ground && f.ph > 20 { f.state = 0; f.ph = 0; }
                }
                _ => { // charge!
                    if f.ph < 30 { f.b.vx = 0.0; f.dir = if dx == 0.0 { 1.0 } else { dx.signum() }; }
                    else { f.b.vx = f.dir * 132.0; }
                    f.b.vy = (f.b.vy + grav * dt).min(220.0);
                    move_body(&mut f.b, dt, TILE, solid);
                    if f.b.hit_wall != 0 { f.state = 3; f.ph = 0; }
                    if f.ph > 110 { f.state = 0; f.ph = 0; }
                }
            }
            if f.state == 3 { // stunned after wall hit
                f.b.vx = 0.0;
                if f.ph > 70 { f.state = 0; f.ph = 0; }
            }
        }
    }
}

/// Foe hitbox for hurting the player (piranha only when emerged).
/// Foe hitbox for hurting the player (piranha only when emerged).
/// Forgiving by 1px per side (classic Mario feel): grazes don't kill,
/// stomps still register (they overlap deeply).
pub fn foe_hitbox(f: &Foe) -> Option<(f32, f32, f32, f32)> {
    if !f.alive { return None; }
    if f.kind == FoeKind::Piranha && f.b.h < 4.0 { return None; }
    Some((f.b.x + 1.0, f.b.y + 1.0, (f.b.w - 2.0).max(1.0), (f.b.h - 2.0).max(1.0)))
}
/// Can this foe be stomped? Spiny & Thwomp hurt; others die to stomps.
#[allow(dead_code)]
pub fn stompable(k: FoeKind) -> bool {
    !matches!(k, FoeKind::Spiny | FoeKind::Thwomp)
}
/// Touch damage to a shell-kicked/rolling shell hurts other foes: shell kills on contact.
pub fn shell_kills(f: &Foe) -> bool { f.kind == FoeKind::Shell && f.b.vx.abs() > 10.0 }

#[cfg(test)]
mod tests {
    use super::*;
    fn open(tx: i32, ty: i32) -> bool { ty >= 10 }
    #[test]
    fn walker_turns_at_wall() {
        let mut f = spawn(FoeKind::Goomba, 20.0, 60.0);
        let wall = |tx: i32, ty: i32| tx >= 6 || open(tx, ty);
        for _ in 0..300 { update_foe(&mut f, 1.0 / 60.0, &wall, 500.0, 60.0, 900.0); }
        assert!(f.b.x < 48.0, "turned around: {}", f.b.x);
    }
    #[test]
    fn piranha_shy() {
        let mut f = spawn(FoeKind::Piranha, 100.0, 60.0);
        for _ in 0..60 { update_foe(&mut f, 1.0 / 60.0, &|_, _| false, 100.0, 60.0, 900.0); }
        assert!(foe_hitbox(&f).is_none(), "stays hidden when player near");
        for _ in 0..200 { update_foe(&mut f, 1.0 / 60.0, &|_, _| false, 500.0, 60.0, 900.0); }
        assert!(foe_hitbox(&f).is_some(), "emerges when player far");
    }
    #[test]
    fn thwomp_slams() {
        let mut f = spawn(FoeKind::Thwomp, 100.0, 20.0);
        let floor = |_: i32, ty: i32| ty >= 8;
        for _ in 0..400 { update_foe(&mut f, 1.0 / 60.0, &floor, 100.0, 100.0, 900.0); }
        assert!(f.state != 0 || f.b.y > 20.0, "thwomp reacted to player below");
    }
}
