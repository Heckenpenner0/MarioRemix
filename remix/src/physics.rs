//! AABB vs tilemap swept-per-axis collision. Units: pixels, seconds.
#[derive(Clone, Copy, Debug)]
pub struct Body {
    pub x: f32, pub y: f32, pub w: f32, pub h: f32,
    pub vx: f32, pub vy: f32,
    pub on_ground: bool,
    pub hit_head: bool,
    pub hit_wall: i32, // -1 left, +1 right, 0 none
}
impl Body {
    pub fn new(x: f32, y: f32, w: f32, h: f32) -> Self {
        Body { x, y, w, h, vx: 0.0, vy: 0.0, on_ground: false, hit_head: false, hit_wall: 0 }
    }
    pub fn cx(&self) -> f32 { self.x + self.w * 0.5 }
    pub fn feet(&self) -> f32 { self.y + self.h }
}
/// Move body, resolving X then Y against solid tiles. `solid(tx,ty)` in tile coords.
pub fn move_body(b: &mut Body, dt: f32, tile: f32, solid: &dyn Fn(i32, i32) -> bool) {
    b.hit_head = false; b.hit_wall = 0;
    // --- X axis (with 1-tall auto-step for grounded bodies) ---
    b.x += b.vx * dt;
    // auto-step: walking into a single protruding tile with headroom steps up
    // (spikes are non-solid so design keeps them out of step headrooms)
    if b.vx != 0.0 && b.on_ground {
        let tx = if b.vx > 0.0 { ((b.x + b.w) / tile) as i32 } else { (b.x / tile) as i32 };
        let y0 = (b.y / tile) as i32; let y1 = ((b.y + b.h - 0.01) / tile) as i32;
        // step iff ONLY the bottom (feet) row is blocked, rows above are free,
        // and the headroom row above the body is free
        if y1 - y0 >= 1 && solid(tx, y1) && !solid(tx, y0 - 1) {
            let mut ok = true;
            for ty in y0..y1 {
                if solid(tx, ty) { ok = false; break; }
            }
            if ok {
                b.y -= tile;
            }
        }
    }
    if b.vx > 0.0 {
        let tx = ((b.x + b.w) / tile) as i32;
        let y0 = (b.y / tile) as i32; let y1 = ((b.y + b.h - 0.01) / tile) as i32;
        for ty in y0..=y1 {
            if solid(tx, ty) {
                b.x = tx as f32 * tile - b.w - 0.01; b.vx = 0.0; b.hit_wall = 1; break;
            }
        }
    } else if b.vx < 0.0 {
        let tx = (b.x / tile) as i32;
        let y0 = (b.y / tile) as i32; let y1 = ((b.y + b.h - 0.01) / tile) as i32;
        for ty in y0..=y1 {
            if solid(tx, ty) {
                b.x = (tx + 1) as f32 * tile + 0.01; b.vx = 0.0; b.hit_wall = -1; break;
            }
        }
    }
    // --- Y axis ---
    b.y += b.vy * dt;
    b.on_ground = false;
    if b.vy >= 0.0 {
        let ty = ((b.y + b.h) / tile) as i32;
        let x0 = ((b.x + 0.5) / tile) as i32; let x1 = ((b.x + b.w - 0.5) / tile) as i32;
        for tx in x0..=x1 {
            if solid(tx, ty) {
                b.y = ty as f32 * tile - b.h; b.vy = 0.0; b.on_ground = true; break;
            }
        }
    } else if b.vy < 0.0 {
        let ty = (b.y / tile) as i32;
        let x0 = ((b.x + 0.5) / tile) as i32; let x1 = ((b.x + b.w - 0.5) / tile) as i32;
        for tx in x0..=x1 {
            if solid(tx, ty) {
                b.y = (ty + 1) as f32 * tile + 0.01; b.vy = 0.0; b.hit_head = true; break;
            }
        }
    }
}
pub fn overlap(a: (f32, f32, f32, f32), b: (f32, f32, f32, f32)) -> bool {
    a.0 < b.0 + b.2 && a.0 + a.2 > b.0 && a.1 < b.1 + b.3 && a.1 + a.3 > b.1
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn falls_and_lands() {
        let mut b = Body::new(4.0, 0.0, 6.0, 6.0);
        b.vy = 50.0;
        let solid = |tx: i32, ty: i32| ty >= 4 && tx >= 0 && tx < 10;
        for _ in 0..120 { b.vy += 900.0 * (1.0 / 60.0); move_body(&mut b, 1.0 / 60.0, 8.0, &solid); }
        assert!(b.on_ground);
        assert!((b.feet() - 32.0).abs() < 0.5);
    }
    #[test]
    fn wall_stops() {
        let mut b = Body::new(0.0, 24.0, 6.0, 6.0);
        b.vx = 100.0;
        let solid = |tx: i32, ty: i32| tx >= 4 && ty >= 0 && ty < 8;
        let mut seen = 0;
        for _ in 0..60 { move_body(&mut b, 1.0 / 60.0, 8.0, &solid); seen |= b.hit_wall; }
        assert_eq!(seen, 1);
        assert!(b.x + b.w <= 32.5);
    }
}
