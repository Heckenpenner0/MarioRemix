//! Player: fused SMW + SML2 + SM64 moveset.
//! SMW: run, variable jump, spin jump, duck. SML2: bunny-ear hover.
//! SM64 (adapted from n64decomp/sm64 US constants, ratios preserved):
//! single 42+0.25fv / double 52+0.25fv / triple 69 (needs speed) /
//! long jump (slide+jump, flat arc, x1.5 speed) / backflip (duck+jump, 62 up) /
//! wall kick (62 up, min forward) / ground pound (hover then -50 slam) /
//! dive (B in air at speed). All text/UX English only.
use crate::level::{Parsed, TILE};
use crate::physics::{Body, move_body};

#[derive(Clone, Copy, Default)]
pub struct Input {
    pub left: bool, pub right: bool, pub up: bool, pub down: bool,
    pub jump: bool, pub run: bool, pub start: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Form { Small, Super, Fire, Bunny, Ball }

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Air { Jump1, Jump2, Jump3, Long, Flip, Kick, Pound, Dive, Hover, Fall }

pub struct Player {
    pub b: Body,
    pub form: Form,
    pub face: f32,
    pub duck: bool,
    pub coyote: u8,
    pub buf: u8,
    pub chain: u8,          // SM64 jump chain: 0 none, 1 single done, 2 double done
    pub chain_win: u8,      // landing window for chaining
    pub air: Air,
    pub wall_dir: f32,      // last air-touched wall side
    pub wall_t: u8,         // frames since air wall touch
    pub pound_t: u8,
    pub pound_impact: bool, // set on slam landing (game breaks tiles/shockwave)
    pub dive_t: u8,
    pub slide_t: u8,
    pub iframes: u8,
    pub star_t: u16,
    pub dead: bool,
    pub jump_holding: bool,
    prev_jump: bool,
    prev_run: bool,
    prev_down: bool,
}
impl Player {
    pub fn new(x: f32, y: f32) -> Self {
        let mut b = Body::new(x, y, 10.0, 12.0);
        b.vy = 0.0;
        Player { b, form: Form::Small, face: 1.0, duck: false,
            coyote: 0, buf: 0, chain: 0, chain_win: 0, air: Air::Fall,
            wall_dir: 0.0, wall_t: 255, pound_t: 0, pound_impact: false,
            dive_t: 0, slide_t: 0, iframes: 0, star_t: 0, dead: false,
            jump_holding: false, prev_jump: false, prev_run: false, prev_down: false }
    }
    pub fn apply_form(&mut self) {
        let (w, h) = match self.form {
            Form::Small => (10.0, 12.0),
            _ => (12.0, 24.0),
        };
        let feet = self.b.feet();
        self.b.w = w; self.b.h = if self.duck { h * 0.55 } else { h };
        self.b.y = feet - self.b.h;
    }
    fn jump_v0(&self, mult: f32) -> f32 { (235.0 + self.b.vx.abs() * 0.35) * mult }

    pub fn update(&mut self, inp: &Input, dt: f32, lv: &Parsed, lowgrav: bool) {
        if self.dead { return; }
        self.pound_impact = false;
        let jump_edge = inp.jump && !self.prev_jump;
        let run_edge = inp.run && !self.prev_run;
        let down_edge = inp.down && !self.prev_down;
        self.prev_jump = inp.jump; self.prev_run = inp.run; self.prev_down = inp.down;
        self.jump_holding = inp.jump;
        if jump_edge { self.buf = 8; } else if self.buf > 0 { self.buf -= 1; }
        if self.iframes > 0 { self.iframes -= 1; }
        if self.star_t > 0 { self.star_t -= 1; }

        let g_up = if lowgrav { 360.0 } else { 650.0 };
        let g_dn = if lowgrav { 990.0 } else { 1800.0 };
        let dir = (inp.right as i32 - inp.left as i32) as f32;
        if dir != 0.0 { self.face = dir; }
        let running = inp.run;
        let max_spd = if running { 105.0 } else { 55.0 };

        let pounding = self.air == Air::Pound;
        let diving = self.air == Air::Dive;

        // --- duck / slide ---
        let want_duck = inp.down && self.b.on_ground && !pounding;
        if want_duck && !self.duck { self.duck = true; self.apply_form(); }
        if !want_duck && self.duck && self.b.vx.abs() < 70.0 {
            self.duck = false; self.apply_form();
        }
        let sliding = self.duck && self.b.vx.abs() >= 70.0;
        self.slide_t = if sliding { self.slide_t.saturating_add(1) } else { 0 };

        // --- horizontal ---
        if !pounding {
            if self.b.on_ground {
                if sliding {
                    self.b.vx -= self.b.vx.signum() * 150.0 * dt;
                    if dir != 0.0 { self.b.vx += dir * 60.0 * dt; }
                } else if self.duck {
                    self.b.vx -= self.b.vx.signum() * 600.0_f32.min(self.b.vx.abs() / dt.max(0.0001) * dt) * dt * 0.0
                        - self.b.vx * 8.0 * dt;
                } else if dir != 0.0 {
                    let acc = if running { 650.0 } else { 500.0 };
                    // skid
                    if self.b.vx.signum() == -dir && self.b.vx.abs() > 20.0 {
                        self.b.vx += dir * 900.0 * dt;
                    } else {
                        self.b.vx += dir * acc * dt;
                    }
                    if self.b.vx.abs() > max_spd { self.b.vx = dir * max_spd; }
                } else {
                    let f = 600.0 * dt;
                    if self.b.vx.abs() <= f { self.b.vx = 0.0; } else { self.b.vx -= self.b.vx.signum() * f; }
                }
            } else if !diving {
                // air control (SM64-ish: limited)
                if dir != 0.0 {
                    self.b.vx += dir * 350.0 * dt;
                    let cap = max_spd + 30.0;
                    if self.b.vx.abs() > cap { self.b.vx = dir * cap; }
                }
            }
        }

        // --- SM64 dive: B-tap in air at speed (no fire form) ---
        if run_edge && !self.b.on_ground && !pounding && !diving && self.form != Form::Fire
            && self.b.vx.abs() > 80.0 && self.air != Air::Hover {
            self.air = Air::Dive; self.dive_t = 0;
            self.b.vx = self.face * 150.0;
            self.b.vy = self.b.vy.max(40.0);
        }
        // --- ground pound: down in air ---
        if down_edge && !self.b.on_ground && !pounding && !diving {
            self.air = Air::Pound; self.pound_t = 0;
            self.b.vx = 0.0; self.b.vy = -60.0;
        }
        if diving {
            self.dive_t += 1;
            self.b.vy = (self.b.vy + 500.0 * dt).min(200.0);
        }
        if pounding {
            self.pound_t += 1;
            if self.pound_t < 12 { self.b.vy = 0.0; }
            else { self.b.vy = 380.0; }
        }

        // --- jumping ---
        let can_jump = self.b.on_ground || self.coyote > 0;
        if self.buf > 0 && !pounding {
            // wall kick (SM64: A during wall-hit window, airborne)
            if !self.b.on_ground && self.wall_t < 10 && dir != 0.0 && dir == -self.wall_dir {
                self.buf = 0; self.wall_t = 255;
                self.air = Air::Kick;
                self.b.vx = -self.wall_dir * 110.0;
                self.b.vy = -280.0;
                self.face = -self.wall_dir;
            } else if can_jump {
                self.buf = 0; self.coyote = 0;
                let spd = self.b.vx.abs();
                if self.duck && sliding {
                    // SM64 long jump: slide + A, x1.5 capped, flat arc
                    self.air = Air::Long;
                    self.b.vy = -193.0;
                    self.b.vx = (self.b.vx * 1.5).clamp(-160.0, 160.0);
                    self.duck = false; self.apply_form();
                    self.chain = 0;
                } else if self.duck && spd < 40.0 {
                    // SM64 backflip: duck + A while slow
                    self.air = Air::Flip;
                    self.b.vy = -300.0;
                    self.b.vx = -self.face * 30.0;
                    self.duck = false; self.apply_form();
                    self.chain = 0;
                } else if self.chain_win > 0 && self.chain == 1 {
                    // double jump
                    self.air = Air::Jump2;
                    self.b.vy = -self.jump_v0(1.12);
                    self.chain = 2; self.chain_win = 0;
                    self.duck = false; self.apply_form();
                } else if self.chain_win > 0 && self.chain == 2 && spd > 60.0 {
                    // SM64 triple jump: needs speed, fixed big launch
                    self.air = Air::Jump3;
                    self.b.vy = -310.0;
                    self.b.vx *= 0.85;
                    self.chain = 0; self.chain_win = 0;
                    self.duck = false; self.apply_form();
                } else {
                    self.air = Air::Jump1;
                    self.b.vy = -self.jump_v0(1.0);
                    self.chain = 1; self.chain_win = 0;
                    self.duck = false; self.apply_form();
                }
            }
        }

        // --- gravity ---
        if self.b.on_ground {
            if self.b.vy > 0.0 { self.b.vy = 0.0; }
        } else if pounding && self.pound_t >= 12 {
            // slam: no gravity, fixed fall
        } else if diving {
            // handled above
        } else {
            let rising = self.b.vy < 0.0;
            let mut g = if rising { g_up } else { g_dn };
            if self.air == Air::Long && rising { g = if lowgrav { 220.0 } else { 380.0 }; }
            // SM64 variable jump: release cuts ascent
            if rising && !inp.jump && self.b.vy < -80.0 { g = g_dn; }
            // SML2 bunny hover: hold jump while falling
            if self.form == Form::Bunny && !rising && inp.jump && self.air != Air::Pound {
                self.air = Air::Hover;
                self.b.vy = (self.b.vy + g * dt).min(22.0);
            } else {
                if self.air == Air::Hover { self.air = Air::Fall; }
                self.b.vy = (self.b.vy + g * dt).min(240.0);
            }
        }

        let was_ground = self.b.on_ground;
        let was_air = self.air;
        move_body(&mut self.b, dt, TILE, &|tx, ty| lv.solid_at(tx, ty));
        // head bump info consumed by game.rs via hit_head + head tile lookup

        if self.b.on_ground {
            self.coyote = 6;
            self.wall_t = 255;
            if !was_ground {
                // landing: pound impact / dive slide / chain window
                if was_air == Air::Pound { self.pound_impact = true; }
                if was_air == Air::Dive {
                    self.slide_t = 0; self.duck = true; self.apply_form();
                    if self.b.vx.abs() < 40.0 { self.duck = false; self.apply_form(); }
                }
                if matches!(was_air, Air::Jump1 | Air::Jump2) {
                    self.chain_win = 7;
                } else { self.chain = 0; }
                if !matches!(was_air, Air::Dive) { self.air = Air::Fall; }
                else if self.b.vx.abs() < 40.0 { self.air = Air::Fall; }
            }
        } else if was_ground {
            self.coyote = self.coyote.saturating_sub(1);
        }
        if self.chain_win > 0 { self.chain_win -= 1; }
        // air wall tracking for wall kick
        if !self.b.on_ground && self.b.hit_wall != 0 {
            self.wall_dir = self.b.hit_wall as f32;
            self.wall_t = 0;
        } else if self.wall_t < 250 { self.wall_t += 1; }
        // dive ends on ground slow
        if self.air == Air::Dive && self.b.on_ground && self.b.vx.abs() < 40.0 {
            self.air = Air::Fall; self.duck = false; self.apply_form();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::level::parse;
    fn flat() -> Parsed {
        parse(&[
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "                                                            ",
            "S                                                           ",
            "############################################################",
        ])
    }
    fn run_frames(p: &mut Player, lv: &Parsed, n: u32, inp: &Input) {
        for _ in 0..n { p.update(inp, 1.0 / 60.0, lv, false); }
    }
    #[test]
    fn runs_and_jumps() {
        let lv = flat();
        let (sx, sy) = lv.spawn;
        let mut p = Player::new(sx, sy);
        let go = Input { right: true, run: true, ..Default::default() };
        run_frames(&mut p, &lv, 120, &go);
        assert!(p.b.vx > 90.0, "run speed {}", p.b.vx);
        assert!(p.b.on_ground);
        let mut j = go; j.jump = true;
        p.update(&j, 1.0 / 60.0, &lv, false);
        assert!(p.b.vy < -200.0, "jump launches {}", p.b.vy);
    }
    #[test]
    fn triple_jump_chains_with_speed() {
        let lv = flat();
        let (sx, sy) = lv.spawn;
        let mut p = Player::new(sx, sy);
        let go = Input { right: true, run: true, ..Default::default() };
        run_frames(&mut p, &lv, 120, &go);
        // single
        let mut j = go; j.jump = true;
        p.update(&j, 1.0 / 60.0, &lv, false);
        assert_eq!(p.air, Air::Jump1);
        let mut idle = go; idle.jump = false;
        // wait for landing (run until grounded, capped)
        for _ in 0..240 { p.update(&idle, 1.0 / 60.0, &lv, false); if p.b.on_ground { break; } }
        assert!(p.b.on_ground);
        // double within window
        p.update(&j, 1.0 / 60.0, &lv, false);
        assert_eq!(p.air, Air::Jump2, "chained double");
        for _ in 0..300 { p.update(&idle, 1.0 / 60.0, &lv, false); if p.b.on_ground { break; } }
        assert!(p.b.on_ground);
        p.update(&j, 1.0 / 60.0, &lv, false);
        assert_eq!(p.air, Air::Jump3, "chained triple at speed");
        assert!(p.b.vy < -300.0);
    }
    #[test]
    fn ground_pound_slams() {
        let lv = flat();
        let (sx, sy) = lv.spawn;
        let mut p = Player::new(sx, sy);
        let go = Input { right: true, run: true, ..Default::default() };
        run_frames(&mut p, &lv, 60, &go);
        let mut j = go; j.jump = true;
        p.update(&j, 1.0 / 60.0, &lv, false);
        let mut air = Input { right: true, run: true, ..Default::default() };
        run_frames(&mut p, &lv, 20, &air);
        assert!(!p.b.on_ground);
        air.down = true;
        p.update(&air, 1.0 / 60.0, &lv, false);
        assert_eq!(p.air, Air::Pound);
        air.down = false;
        for _ in 0..240 { p.update(&air, 1.0 / 60.0, &lv, false); if p.b.on_ground { break; } }
        assert!(p.pound_impact, "slam registers impact");
    }
    #[test]
    fn backflip_from_duck() {
        let lv = flat();
        let (sx, sy) = lv.spawn;
        let mut p = Player::new(sx, sy);
        let idle = Input::default();
        run_frames(&mut p, &lv, 30, &idle);
        let mut d = Input { down: true, jump: true, ..Default::default() };
        p.update(&d, 1.0 / 60.0, &lv, false);
        d.jump = false;
        assert_eq!(p.air, Air::Flip, "duck+jump backflips");
        assert!(p.b.vy < -290.0);
    }
}
