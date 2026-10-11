//! Stateful verification AI: proves every level is completable with basic moves.
//! Two-tier hops: short stomps up close, full jumps to fly over, panic hops,
//! spike scans, and thwomp run-throughs (never jump under a thwomp).
use crate::enemy::{foe_hitbox, FoeKind};
use crate::game::Game;
use crate::level::TILE;
use crate::player::Input;

/// Would a full Jump1 from here clear everything ahead? Simulates the
/// parabola and tests the body box along it. A blocked arc only head-bonks
/// (lv3 Starry Vault platform), so the bot short-hops instead; a clear arc
/// flies over block + foe + pit (lv5 Macro), so it goes full.
fn full_arc_fits(g: &Game, cx: f32, feet: f32, vx: f32, w: f32, h: f32) -> bool {
    let spd = vx.abs().max(60.0);
    let v0 = 235.0 + spd * 0.35;
    let (g_up, g_dn) = if g.lowgrav { (360.0, 990.0) } else { (650.0, 1800.0) };
    let t_apex = v0 / g_up;
    let apex = v0 * t_apex - 0.5 * g_up * t_apex * t_apex;
    let mut dx = 8.0;
    while dx < 110.0 {
        let t = dx / spd;
        let rise = if t <= t_apex { v0 * t - 0.5 * g_up * t * t }
            else { apex - 0.5 * g_dn * (t - t_apex) * (t - t_apex) };
        if rise < -40.0 { break; } // landed; rest is ground running
        let y_top = feet - rise - h;
        let pts = [cx + dx - w * 0.5, cx + dx + w * 0.5];
        for ox in pts {
            for oy in [y_top, y_top + h * 0.5, feet - rise] {
                if g.lv.solid_at((ox / TILE) as i32, (oy / TILE) as i32) {
                    return false;
                }
            }
        }
        dx += 8.0;
    }
    true
}
pub struct Bot { hold: u8, wait: u8, pj: bool }
impl Bot {
    pub fn new() -> Self { Bot { hold: 0, wait: 0, pj: false } }
    pub fn input(&mut self, g: &Game) -> Input {
        let mut inp = Input { right: true, run: true, ..Default::default() };
        let p = &g.pl;
        if p.dead { self.hold = 0; self.pj = false; return Input::default(); }
        let px = p.b.cx();
        if p.b.on_ground && self.hold > 0 { self.hold = 0; }
        let ahead = px + p.face * 40.0;
        let tx = (ahead / TILE) as i32;
        let feet = p.b.feet();
        let ty_feet = (feet / TILE) as i32;
        let ty_head = (p.b.y / TILE) as i32;
        // wall scan: range ahead (starts close so we never go blind pressed
        // against a wall), records nearest face distance, top, and height.
        // 1-tall steps are ignored: physics auto-step handles them.
        let mut wall_dist = 9999.0;
        let mut wall_top = 0.0;
        let mut wall_rows = 0;
        for dpx in (8..56).step_by(4) {
            let sx = px + p.face * dpx as f32;
            let cx = (sx / TILE) as i32;
            let mut top = 9999.0;
            let mut rows = 0;
            let mut bottom = false;
            for ty in ty_head..=ty_feet {
                if g.lv.solid_at(cx, ty) {
                    rows += 1;
                    let y = ty as f32 * TILE;
                    if y < top { top = y; }
                    if ty == ty_feet { bottom = true; }
                }
            }
            // ignore single bottom-row blocks: auto-step lifts us over
            if rows == 1 && bottom { continue; }
            if rows > 0 {
                let face = if p.face > 0.0 {
                    cx as f32 * TILE - (p.b.x + p.b.w)
                } else {
                    p.b.x - (cx as f32 * TILE + TILE)
                };
                if face < wall_dist {
                    wall_dist = face;
                    wall_top = top;
                    wall_rows = rows;
                }
            }
        }
        let wall = wall_dist < 9000.0;
        let _ = wall_rows;
        // precise gap edge
        let edge_tx = ((px + p.face * (p.b.w * 0.5 + 2.0)) / TILE) as i32;
        let mut gap = !g.lv.solid_at(edge_tx, ty_feet + 1) && !g.lv.solid_at(edge_tx, ty_feet + 2);
        if !gap {
            gap = true;
            for dx in 0..3 {
                for dy in 0..4 {
                    if g.lv.solid_at(tx + dx, ty_feet + 1 + dy) { gap = false; break; }
                }
                if !gap { break; }
            }
        }
        // spikes in body rows AND below feet ahead: jump early (covers jumping
        // off ledges/stems onto spike rows, and running at low spikes)
        let mut spike = false;
        for dpx in (8..56).step_by(4) {
            let txx = ((px + p.face * dpx as f32) / TILE) as i32;
            for ty in ty_head..=ty_feet + 2 {
                if g.lv.at(txx, ty) == crate::level::Tile::Spike { spike = true; break; }
            }
            if spike { break; }
        }
        // danger beyond a wall (spikes or a pit in the landing zone): decides
        // whether the wall jump hops SHORT onto the wall top or blasts over
        let mut danger_beyond = false;
        if wall {
            let wx = px + p.face * wall_dist;
            for dpx in (0..100).step_by(4) {
                let txb = ((wx + p.face * dpx as f32) / TILE) as i32;
                for ty in ty_feet - 1..=ty_feet + 2 {
                    if g.lv.at(txb, ty) == crate::level::Tile::Spike {
                        danger_beyond = true;
                        break;
                    }
                }
                if danger_beyond { break; }
            }
            if !danger_beyond {
                let mut gap_run = 0;
                for dpx in (8..64).step_by(4) {
                    let txb = ((wx + p.face * dpx as f32) / TILE) as i32;
                    if !g.lv.solid_at(txb, ty_feet + 1) && !g.lv.solid_at(txb, ty_feet + 2) {
                        gap_run += 1;
                    }
                }
                if gap_run >= 3 { danger_beyond = true; }
            }
        }
        // foe analysis
        let mut foe_far = false;   // fly over (full jump)
        let mut foe_near = false;  // stomp (short hop), unless spiny/thwomp
        let mut foe_danger = false; // spiny/thwomp close: full jump only
        let mut thwomp_ahead = false; // run through, never coin-hop near one
        for f in &g.foes {
            if !f.alive { continue; }
            // no hitbox (hidden piranha): not a threat, ignore completely
            let Some((fx, fy, fw, fh)) = foe_hitbox(f) else { continue; };
            let d = fx + fw * 0.5 - px;
            let vd = (fy - p.b.y).abs();
            // wide dead zone: shake jitters d across 0 mid-crossing, and the
            // column isn't clear until centers separate by half-widths+margin
            // (10+5+3). Without it a coin-hop fires into the hover box edge.
            if f.kind == FoeKind::Thwomp && d * p.face > -18.0 && d.abs() < 95.0 && vd < 60.0 {
                thwomp_ahead = true;
            }
            // run-through owns thwomps: the telegraph shake jitters d across
            // 0 and would panic-jump us into the hover box mid-crossing
            if thwomp_ahead && f.kind == FoeKind::Thwomp { continue; }
            // flyers: run under only with honest 2px clearance, else fly over
            let flyer_clear = |fy: f32, fh: f32| {
                p.b.y >= fy + fh + 2.0 || fy >= p.b.y + p.b.h + 2.0
            };
            if d * p.face > 0.0 && d.abs() < 42.0 && vd < 36.0 {
                match f.kind {
                    FoeKind::Spiny | FoeKind::Thwomp => foe_danger = true,
                    // hopping octopus: always full jump, stomp from height
                    FoeKind::Octopus => foe_far = true,
                    // emerged plant: full jump over the pipe zone
                    FoeKind::Piranha => foe_far = true,
                    FoeKind::Bat | FoeKind::Bird | FoeKind::Fish => {
                        if !flyer_clear(fy, fh) { foe_far = true; }
                    }
                    _ => {
                        if d.abs() < 26.0 { foe_near = true; } else { foe_far = true; }
                    }
                }
            }
            if d.abs() < 14.0 && vd < 20.0 {
                match f.kind {
                    FoeKind::Spiny | FoeKind::Thwomp => foe_danger = true,
                    FoeKind::Octopus | FoeKind::Piranha => foe_far = true,
                    FoeKind::Bat | FoeKind::Bird | FoeKind::Fish => {
                        if !flyer_clear(fy, fh) { foe_danger = true; }
                    }
                    _ => foe_near = true,
                }
            }
        }
        let mut want = false;
        for dy in 1..=3 {
            for dx in 0..2 {
                match g.lv.at(tx + dx, ty_head - dy) {
                    crate::level::Tile::Gold | crate::level::Tile::Dragon
                    | crate::level::Tile::Red => { want = true; break; }
                    _ => {}
                }
            }
        }
        // low gravity sails far: shorten holds there
        let (short, full) = if g.lowgrav { (6, 12) } else { (10, 20) };
        // wall jump: landable top + danger beyond -> SHORT onto the top from
        // range (re-jump from there), FULL blast-over when already close.
        // Otherwise FULL to clear. SHORT stages onto low tops (rise <= 20)
        // from range; higher step-ups (lv5 Macro block: 24px) need FULL from
        // range or the hop face-plants and slides.
        let wall_hold = if wall && (feet - wall_top) <= 34.0 && danger_beyond {
            if wall_dist > 16.0 && (feet - wall_top) <= 20.0 { short } else { full }
        } else {
            full
        };
        // low ceiling with a foe crowding us: hops only bonk, so back up
        // (when ground behind) and let the patroller walk past
        let ptx = (px / TILE) as i32;
        let ceiling = g.lv.solid_at(ptx, ty_head - 1) || g.lv.solid_at(ptx, ty_head - 2);
        let mut retreat = false;
        if foe_near && ceiling {
            let behind_tx = ((px - p.face * (p.b.w * 0.5 + 3.0)) / TILE) as i32;
            if g.lv.solid_at(behind_tx, ty_feet + 1) {
                retreat = true;
            }
        }
        // foe reaching into the arc band of a forced (gap/spike) jump: stop
        // early (38px before the edge), wait for a window, then run-up jump.
        // (timeout: go anyway.) Grounded walkers under a high arc are ignored.
        let mut blocker = false;
        if gap || spike {
            for f in &g.foes {
                if !f.alive { continue; }
                if let Some((fx, fy, fw, fh)) = foe_hitbox(f) {
                    // center must be ahead: a foe behind us can't meet a
                    // forward jump (lv10 Turtle: waiting on a trailing
                    // octopus = death by standing still)
                    if f.kind != FoeKind::Thwomp
                        && fx + fw * 0.5 > px && fx < px + 80.0
                        && fy < feet - 15.0 && fy + fh > feet - 60.0
                    {
                        blocker = true;
                        break;
                    }
                }
            }
        }
        if blocker { self.wait = self.wait.saturating_add(1); }
        else { self.wait = 0; }
        // hole/spike-line edge ahead (for early stopping)
        let mut edge_px = px + 44.0;
        if gap || spike {
            for dpx in (4..76).step_by(4) {
                let txe = ((px + p.face * dpx as f32) / TILE) as i32;
                let hole = !g.lv.solid_at(txe, ty_feet + 1) && !g.lv.solid_at(txe, ty_feet + 2);
                let spk = g.lv.at(txe, ty_feet) == crate::level::Tile::Spike
                    || g.lv.at(txe, ty_feet + 1) == crate::level::Tile::Spike;
                if hole || spk {
                    edge_px = px + p.face * dpx as f32;
                    break;
                }
            }
        }
        // trailer: a foe closing from behind while we wait at an edge.
        // Holding still for it is suicide (lv10 Turtle octopus); jump now.
        let mut trailer = false;
        for f in &g.foes {
            if !f.alive || f.kind == FoeKind::Thwomp { continue; }
            if let Some((fx, fy, fw, _fh)) = foe_hitbox(f) {
                let d = fx + fw * 0.5 - px;
                if d * p.face < 0.0 && d.abs() < 24.0 && (fy - p.b.y).abs() < 30.0 {
                    trailer = true;
                    break;
                }
            }
        }
        // waiting states: approach (run in, no jump yet), hold (stop, wait),
        // overcooked (too close: jump anyway)
        let waiting = (gap || spike) && blocker && self.wait < 150;
        let lead = px + p.b.w * 0.5 + p.face * p.b.w * 0.5; // leading edge
        let dist_edge = (edge_px - lead) * p.face;
        let approach = waiting && dist_edge > 38.0 && !trailer;
        let hold_win = waiting && dist_edge > 4.0 && !approach && !trailer;
        // low ceiling ahead along the arc (lv3 Starry Vault: a full jump
        // under the platform only head-bonks, then a held-input stomp bounce
        // re-launches into the pit). Cap far-foe jumps to short stomps here.
        let mut low_ceil_ahead = false;
        'ceil: for dpx in (8..56).step_by(8) {
            let txc = ((px + p.face * dpx as f32) / TILE) as i32;
            for dy in 1..=4 {
                if g.lv.solid_at(txc, ty_head - dy) {
                    low_ceil_ahead = true;
                    break 'ceil;
                }
            }
        }
        // only cap to a short hop when the full arc is actually doomed
        let arc_ok = !foe_far || !low_ceil_ahead
            || full_arc_fits(g, px, feet, p.b.vx * p.face, p.b.w, p.b.h);
        if p.b.on_ground {
            if hold_win {
                inp.right = false; // hold before the edge; jump when clear
            } else if retreat {
                inp.right = false;
                inp.left = p.face > 0.0;
                if p.face < 0.0 { inp.right = true; }
            } else if thwomp_ahead {
                // run through; the telegraph + abort window keeps this safe
            } else if foe_near || (foe_far && !arc_ok) {
                // short stomp hop only onto clean ground; with spikes/a pit in
                // the landing zone go full to fly over both
                let mut danger_landing = false;
                for dpx in (8..64).step_by(4) {
                    let txl = ((px + p.face * dpx as f32) / TILE) as i32;
                    for ty in ty_head..=ty_feet + 2 {
                        if g.lv.at(txl, ty) == crate::level::Tile::Spike {
                            danger_landing = true;
                            break;
                        }
                    }
                    if danger_landing { break; }
                }
                if !danger_landing {
                    let mut run = 0;
                    for dpx in (8..64).step_by(4) {
                        let txl = ((px + p.face * dpx as f32) / TILE) as i32;
                        if !g.lv.solid_at(txl, ty_feet + 1) && !g.lv.solid_at(txl, ty_feet + 2) {
                            run += 1;
                            if run >= 2 { danger_landing = true; break; }
                        } else {
                            run = 0;
                        }
                    }
                }
                inp.jump = true;
                self.hold = if danger_landing && arc_ok { full } else { short };
            } else if wall {
                inp.jump = true; self.hold = wall_hold;
            } else if (gap || spike) && waiting && !approach && !hold_win {
                // overcooked past the hold point: jump anyway (emergency)
                inp.jump = true; self.hold = full;
            } else if gap || (foe_far && arc_ok) || foe_danger || (want && !thwomp_ahead) || spike {
                if (gap || spike) && approach {
                    // still approaching the hold point: run in, no jump yet
                } else {
                    inp.jump = true; self.hold = full;  // full jump
                }
            }
        } else if !p.b.on_ground && self.hold > 0 {
            inp.jump = true;
            self.hold -= 1;
        }
// re-arm: a jump held across landing never produces the fresh edge
        // the player needs, so release exactly one grounded frame (lv5 Macro:
        // holding J into an ant with no edge = death march).
        if p.b.on_ground && inp.jump && self.pj {
            inp.jump = false;
        }
        self.pj = inp.jump;
        inp
    }
}
/// Run bot until outcome/death/timeout. Returns frames used or None.
pub fn play_level(g: &mut Game, prog: &mut crate::game::Progress, max_frames: u64) -> Option<u64> {
    let mut bot = Bot::new();
    let mut last_run = false;
    for _ in 0..max_frames {
        let inp = bot.input(g);
        g.update_shoot_edge(&inp, last_run);
        last_run = inp.run;
        g.update(&inp, prog);
        if g.outcome != crate::game::Outcome::None { return Some(g.tick); }
        if g.game_over { return None; }
        if g.pl.dead && g.dead_t > 160 { return None; }
    }
    None
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::Progress;
    use crate::level;
    #[test]
    fn bot_completes_every_normal_level() {
        let metas = level::levels();
        for (i, m) in metas.iter().enumerate() {
            if m.zone == 6 { continue; } // castle needs boss fight
            let mut prog = Progress::new(metas.len());
            let mut g = Game::new(i, &prog);
            let r = play_level(&mut g, &mut prog, 60 * 150);
            assert!(r.is_some(), "bot failed level {} {}", i, m.name);
        }
    }
}
