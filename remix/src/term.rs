//! Terminal frontend: raw keyboard input + ANSI truecolor framebuffer output.
//! Pure std (configures the tty via `stty`).
use std::io::Write;
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::thread;
use crate::player::Input;
use crate::render::Frame;

pub struct Keys {
    pub inp: Arc<Mutex<Input>>,
    pub quit: Arc<Mutex<bool>>,
}
impl Keys {
    pub fn spawn() -> Self {
        let inp = Arc::new(Mutex::new(Input::default()));
        let quit = Arc::new(Mutex::new(false));
        let t_inp = inp.clone(); let t_quit = quit.clone();
        thread::spawn(move || {
            let stdin = std::io::stdin();
            let mut lock = stdin.lock();
            let mut buf = [0u8; 1];
            use std::io::Read;
            let mut esc = 0u8; // 0 none, 1 got ESC, 2 got ESC[
            loop {
                let n = lock.read(&mut buf).unwrap_or(0);
                if n == 0 { thread::sleep(std::time::Duration::from_millis(4)); continue; }
                let b = buf[0];
                let mut gi = t_inp.lock().unwrap();
                if esc == 1 {
                    esc = 0;
                    if b == b'[' { esc = 2; continue; }
                    else { *t_quit.lock().unwrap() = true; continue; }
                } else if esc == 2 {
                    esc = 0;
                    // arrows latch until the main loop clears them after each frame
                    // (holding a key auto-repeats the sequence)
                    match b {
                        b'A' => gi.up = true,
                        b'B' => gi.down = true,
                        b'C' => gi.right = true,
                        b'D' => gi.left = true,
                        _ => {}
                    }
                    continue;
                }
                match b {
                    0x1B => { esc = 1; }
                    b' ' | b'x' | b'X' | b'k' | b'K' => gi.jump = true,
                    b'z' | b'Z' | b'j' | b'J' => gi.run = true,
                    b'a' | b'A' => gi.left = true,
                    b'd' | b'D' => gi.right = true,
                    b'w' | b'W' => gi.up = true,
                    b's' | b'S' => gi.down = true,
                    b'\r' | b'\n' => gi.start = true,
                    b'q' | b'Q' => { *t_quit.lock().unwrap() = true; }
                    _ => {}
                }
            }
        });
        Keys { inp, quit }
    }
    /// Snapshot input; momentary keys (arrows/space) are one-shot: clear after read.
    pub fn take(&self) -> Input {
        let mut gi = self.inp.lock().unwrap();
        let out = *gi;
        gi.jump = false; gi.run = false; gi.start = false; gi.up = false;
        gi.left = false; gi.right = false; gi.down = false;
        out
    }
}

pub fn raw_enable() {
    let _ = Command::new("stty").args(["-icanon", "-echo", "min", "0", "time", "0"]).status();
}
pub fn raw_restore() {
    let _ = Command::new("stty").args(["sane"]).status();
}
pub fn hide_cursor() { print!("\x1b[?25l"); let _ = std::io::stdout().flush(); }
pub fn show_cursor() { print!("\x1b[?25h"); let _ = std::io::stdout().flush(); }

/// Render frame as 96x56 half-block cells. No trailing newline (stable, no scroll).
pub fn present(f: &Frame) {
    let mut s = String::with_capacity(f.w * f.h * 8);
    s.push_str("\x1b[H");
    for cy in (0..f.h).step_by(2) {
        for x in 0..f.w {
            let i0 = (cy * f.w + x) * 3;
            let i1 = ((cy + 1) * f.w + x) * 3;
            let (r0, g0, b0) = (f.buf[i0], f.buf[i0 + 1], f.buf[i0 + 2]);
            let (r1, g1, b1) = if cy + 1 < f.h { (f.buf[i1], f.buf[i1 + 1], f.buf[i1 + 2]) } else { (0, 0, 0) };
            s.push_str(&format!("\x1b[38;2;{};{};{}m\x1b[48;2;{};{};{}m\u{2580}",
                r0, g0, b0, r1, g1, b1));
        }
        s.push_str("\x1b[0m");
        if cy + 2 < f.h { s.push('\n'); }
    }
    let mut o = std::io::stdout().lock();
    let _ = o.write_all(s.as_bytes());
    let _ = o.flush();
}
