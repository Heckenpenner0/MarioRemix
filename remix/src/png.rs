//! Minimal RGBA PNG encoder (zlib stored blocks, no compression).
//! Zero dependencies: CRC32 + Adler32 computed by hand.
pub fn encode_rgba(w: u32, h: u32, px: &[u8]) -> Vec<u8> {
    assert_eq!(px.len(), (w * h * 4) as usize);
    let mut out = Vec::new();
    out.extend_from_slice(&[137, 80, 78, 71, 13, 10, 26, 10]);
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&w.to_be_bytes());
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.push(8); ihdr.push(6); ihdr.push(0); ihdr.push(0); ihdr.push(0);
    chunk(&mut out, b"IHDR", &ihdr);
    // raw scanlines with filter byte 0
    let stride = (w * 4) as usize;
    let mut raw = Vec::with_capacity(((h as usize) * (stride + 1)) + 8);
    for y in 0..h as usize {
        raw.push(0);
        raw.extend_from_slice(&px[y * stride..(y + 1) * stride]);
    }
    // zlib header (CMF=0x78, FLG=0x01) + stored blocks + adler32
    let mut z = vec![0x78, 0x01];
    let mut i = 0;
    while i < raw.len() {
        let n = (raw.len() - i).min(65535);
        let last = i + n >= raw.len();
        z.push(if last { 1 } else { 0 });
        z.extend_from_slice(&(n as u16).to_le_bytes());
        z.extend_from_slice(&(!(n as u16)).to_le_bytes());
        z.extend_from_slice(&raw[i..i + n]);
        i += n;
    }
    let ad = adler32(&raw);
    z.extend_from_slice(&ad.to_be_bytes());
    chunk(&mut out, b"IDAT", &z);
    chunk(&mut out, b"IEND", &[]);
    out
}
fn chunk(out: &mut Vec<u8>, ty: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(ty);
    out.extend_from_slice(data);
    let mut c = crc32(ty);
    c = crc32_update(c, data);
    out.extend_from_slice(&c.to_be_bytes());
}
fn crc_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    for i in 0..256 {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB88320 ^ (c >> 1) } else { c >> 1 };
        }
        t[i] = c;
    }
    t
}
fn crc32(data: &[u8]) -> u32 { crc32_update(0xFFFF_FFFF, data) ^ 0xFFFF_FFFF }
fn crc32_update(mut c: u32, data: &[u8]) -> u32 {
    let t = crc_table();
    for &b in data {
        c = t[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    c ^ 0xFFFF_FFFF
}
fn adler32(d: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for &x in d {
        a = (a + x as u32) % 65521;
        b = (b + a) % 65521;
    }
    (b << 16) | a
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn png_signature_and_roundtrip_size() {
        let px = vec![255u8; 4 * 4 * 4];
        let p = encode_rgba(4, 4, &px);
        assert_eq!(&p[0..8], &[137, 80, 78, 71, 13, 10, 26, 10]);
        assert!(p.len() > 70);
    }
}
