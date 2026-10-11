// Minimal PNG encoder (8-bit RGBA, filter "Up") + integer box-filter downscale. Zero dependencies.
//   encodePng(width, height, rgba) -> Buffer
//   downscale(rgba, w, h, factor)  -> {width, height, data}
import { deflateSync } from 'node:zlib';

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, 'latin1');
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}

export function encodePng(width, height, rgba) {
  const stride = width * 4;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    const ro = y * (stride + 1);
    const so = y * stride;
    raw[ro] = y === 0 ? 0 : 2; // None for first row, Up afterwards
    if (y === 0) for (let i = 0; i < stride; i++) raw[ro + 1 + i] = rgba[so + i];
    else for (let i = 0; i < stride; i++) raw[ro + 1 + i] = (rgba[so + i] - rgba[so - stride + i]) & 255;
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; ihdr[9] = 6; // 8-bit, RGBA
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

export function downscale(rgba, w, h, factor) {
  factor = Math.max(1, factor | 0);
  if (factor === 1) return { width: w, height: h, data: rgba };
  const nw = Math.floor(w / factor), nh = Math.floor(h / factor);
  const out = new Uint8Array(nw * nh * 4);
  const n = factor * factor;
  for (let y = 0; y < nh; y++) {
    for (let x = 0; x < nw; x++) {
      let r = 0, g = 0, b = 0, a = 0;
      for (let dy = 0; dy < factor; dy++) {
        let o = ((y * factor + dy) * w + x * factor) * 4;
        for (let dx = 0; dx < factor; dx++, o += 4) { r += rgba[o]; g += rgba[o + 1]; b += rgba[o + 2]; a += rgba[o + 3]; }
      }
      const d = (y * nw + x) * 4;
      out[d] = (r / n + 0.5) | 0; out[d + 1] = (g / n + 0.5) | 0; out[d + 2] = (b / n + 0.5) | 0; out[d + 3] = (a / n + 0.5) | 0;
    }
  }
  return { width: nw, height: nh, data: out };
}
