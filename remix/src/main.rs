//! Worlds United: playable SMW x SML2 x SM64 crossover remix (English only).
mod bot;
mod enemy; mod font; mod game; mod level;
mod maps1; mod maps2; mod physics; mod player; mod png; mod render; mod sprites; mod term;

use game::{Game, Outcome, Progress};
use player::Input;

enum Screen { Title, Map { cursor: usize }, Play(Game), Ending, GameOver }

fn scale3(f: &render::Frame) -> (u32, u32, Vec<u8>) {
    let (w, h) = (f.w as u32 * 3, f.h as u32 * 3);
    let mut px = vec![0u8; (w * h * 4) as usize];
    for y in 0..f.h { for x in 0..f.w {
        let i = (y * f.w + x) * 3;
        let (r, g, b) = (f.buf[i], f.buf[i + 1], f.buf[i + 2]);
        for dy in 0..3 { for dx in 0..3 {
            let o = (((y * 3 + dy) as u32 * w + (x * 3 + dx) as u32) * 4) as usize;
            px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = 255;
        }}
    }}
    (w, h, px)
}
fn save_png(f: &render::Frame, path: &str) {
    let (w, h, px) = scale3(f);
    std::fs::write(path, png::encode_rgba(w, h, &px)).unwrap();
    println!("wrote {} ({}x{})", path, w, h);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 {
        match args[1].as_str() {
            "--dump" => {
                let li: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(0);
                let out = args.get(3).cloned().unwrap_or_else(|| "shot.png".into());
                let metas = level::levels();
                let mut prog = Progress::new(metas.len());
                let mut g = Game::new(li.min(metas.len() - 1), &prog);
                let idle = Input::default();
                for _ in 0..30 { g.update(&idle, &mut prog); }
                save_png(&render::render_world(&g), &out);
                return;
            }
            "--title" => {
                let metas = level::levels();
                let prog = Progress::new(metas.len());
                save_png(&render::render_title(&prog), args.get(2).cloned().unwrap_or("title.png".into()).as_str());
                return;
            }
            "--map" => {
                let metas = level::levels();
                let prog = Progress::new(metas.len());
                save_png(&render::render_map(&prog, 0, &metas), args.get(2).cloned().unwrap_or("map.png".into()).as_str());
                return;
            }
            "--shots" => {
                let dir = args.get(2).cloned().unwrap_or_else(|| "shots".into());
                std::fs::create_dir_all(&dir).unwrap();
                let metas = level::levels();
                for (i, m) in metas.iter().enumerate() {
                    let mut prog = Progress::new(metas.len());
                    let mut g = Game::new(i, &prog);
                    let idle = Input::default();
                    for _ in 0..90 { g.update(&idle, &mut prog); }
                    // run bot a while for an action shot
                    let mut last_run = false;
                    let mut abot = bot::Bot::new();
                    for _ in 0..600 {
                        let inp = abot.input(&g);
                        g.update_shoot_edge(&inp, last_run);
                        last_run = inp.run;
                        g.update(&inp, &mut prog);
                        if g.outcome != Outcome::None || g.pl.dead { break; }
                    }
                    save_png(&render::render_world(&g), &format!("{}/lv{:02}.png", dir, i));
                    println!("level {} {} reds-seeded ok", i, m.name);
                }
                return;
            }
            "--bot" => {
                let metas = level::levels();
                if let Some(n) = args.get(2).and_then(|s| s.parse::<usize>().ok()) {
                    let mut prog = Progress::new(metas.len());
                    let mut g = Game::new(n.min(metas.len() - 1), &prog);
                    match bot::play_level(&mut g, &mut prog, 60 * 150) {
                        Some(t) => println!("level {} cleared in {} frames ({}s)", n, t, t / 60),
                        None => println!("level {} NOT cleared", n),
                    }
                    return;
                }
                for (i, m) in metas.iter().enumerate() {
                    if m.zone == 6 { println!("level {} {}: boss (bot skips)", i, m.name); continue; }
                    let mut prog = Progress::new(metas.len());
                    let mut g = Game::new(i, &prog);
                    match bot::play_level(&mut g, &mut prog, 60 * 150) {
                        Some(t) => println!("level {:2} {:16} cleared {:5}f gold:{} star:{}",
                            i, m.name, t, prog.gold[m.zone], prog.stars[i]),
                        None => println!("level {:2} {:16} FAILED", i, m.name),
                    }
                }
                return;
            }
            _ => {}
        }
    }
    play_terminal();
}

fn play_terminal() {
    term::raw_enable();
    term::hide_cursor();
    print!("\x1b[2J");
    let keys = term::Keys::spawn();
    let metas = level::levels();
    let mut prog = Progress::new(metas.len());
    let mut screen = Screen::Title;
    let mut last_run = false;
    let frame_dt = std::time::Duration::from_micros(16_666);
    loop {
        let t0 = std::time::Instant::now();
        if *keys.quit.lock().unwrap() { break; }
        let inp = keys.take();
        let start = inp.start;
        match &mut screen {
            Screen::Title => {
                term::present(&render::render_title(&prog));
                if start { screen = Screen::Map { cursor: 0 }; }
            }
            Screen::Map { cursor } => {
                // navigate (skip locked)
                if inp.down || inp.right { *cursor = (*cursor + 1).min(metas.len() - 1); }
                if inp.up || inp.left { *cursor = cursor.saturating_sub(1); }
                term::present(&render::render_map(&prog, *cursor, &metas));
                if start {
                    let i = *cursor;
                    let locked = i > prog.unlocked || (metas[i].zone == 6 && !prog.castle_open());
                    if !locked {
                        last_run = false;
                        screen = Screen::Play(Game::new(i, &prog));
                    }
                }
            }
            Screen::Play(g) => {
                g.update_shoot_edge(&inp, last_run);
                last_run = inp.run;
                g.update(&inp, &mut prog);
                term::present(&render::render_world(g));
                if g.game_over {
                    prog = Progress::new(metas.len());
                    screen = Screen::GameOver;
                } else if g.outcome == Outcome::Clear {
                    let i = g.lv_idx;
                    prog.unlocked = prog.unlocked.max((i + 1).min(metas.len() - 1));
                    if g.clear_t > 150 {
                        screen = Screen::Map { cursor: (i + 1).min(metas.len() - 1) };
                    }
                } else if g.outcome == Outcome::CastleClear && g.clear_t > 220 {
                    screen = Screen::Ending;
                }
            }
            Screen::Ending => {
                term::present(&render::render_ending(prog.score, prog.star_count()));
                if start { screen = Screen::Title; }
            }
            Screen::GameOver => {
                term::present(&render::render_gameover());
                if start { screen = Screen::Title; }
            }
        }
        let el = t0.elapsed();
        if el < frame_dt { std::thread::sleep(frame_dt - el); }
    }
    term::raw_restore();
    term::show_cursor();
    println!();
}
