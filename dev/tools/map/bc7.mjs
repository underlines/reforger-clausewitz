// Block-compression decoders -> RGBA8: BC7 (all 8 modes), BC1 (DXT1), BC2 (DXT3), BC3 (DXT5),
// plus passthrough for R8G8B8A8 / B8G8R8A8 / R8. No sRGB conversion is applied (raw stored values).
//   decodeToRgba({format (dxgi), width, height, data}) -> Uint8Array (width*height*4)

// ---- BC7 tables (D3D11 spec / bc7decomp) ----
const P2 = [
  '0011001100110011', '0001000100010001', '0111011101110111', '0001001100110111',
  '0000000100010011', '0011011101111111', '0001001101111111', '0000000100110111',
  '0000000000010011', '0011011111111111', '0000000101111111', '0000000000010111',
  '0001011111111111', '0000000011111111', '0000111111111111', '0000000000001111',
  '0000100011101111', '0111000100000000', '0000000010001110', '0111001100010000',
  '0011000100000000', '0000100011001110', '0000000010001100', '0111001100110001',
  '0011000100010000', '0000100010001100', '0110011001100110', '0011011001101100',
  '0001011111101000', '0000111111110000', '0111000110001110', '0011100110011100',
  '0101010101010101', '0000111100001111', '0101101001011010', '0011001111001100',
  '0011110000111100', '0101010110101010', '0110100101101001', '0101101010100101',
  '0111001111001110', '0001001111001000', '0011001001001100', '0011101111011100',
  '0110100110010110', '0011110011000011', '0110011010011001', '0000011001100000',
  '0100111001000000', '0010011100100000', '0000001001110010', '0000010011100100',
  '0110110010010011', '0011011011001001', '0110001110011100', '0011100111000110',
  '0110110011001001', '0110001100111001', '0111111010000001', '0001100011100111',
  '0000111100110011', '0011001111110000', '0010001011101110', '0100010001110111',
];
const P3 = [
  '0011001102212222', '0001001122112221', '0000200122112211', '0222002200110111',
  '0000000011221122', '0011001100220022', '0022002211111111', '0011001122112211',
  '0000000011112222', '0000111111112222', '0000111122222222', '0012001200120012',
  '0112011201120112', '0122012201220122', '0011011211221222', '0011200122002220',
  '0001001101121122', '0111001120012200', '0000112211221122', '0022002200221111',
  '0111011102220222', '0001000122212221', '0000001101220122', '0000110022102210',
  '0122012200110000', '0012001211222222', '0110122112210110', '0000011012211221',
  '0022110211020022', '0110011020022222', '0011012201220011', '0000200022112221',
  '0000000211221222', '0222002200120011', '0011001200220222', '0120012001200120',
  '0000111122220000', '0120120120120120', '0120201212010120', '0011220011220011',
  '0011112222000011', '0101010122222222', '0000000021212121', '0022112200221122',
  '0022001100220011', '0220122102201221', '0101222222220101', '0000212121212121',
  '0101010101012222', '0222011102220111', '0002111200021112', '0000211221122112',
  '0222011101110222', '0002111211120002', '0110011001102222', '0000000021122112',
  '0110011022222222', '0022001100110022', '0022112211220022', '0000000000002112',
  '0002000100020001', '0222122202221222', '0101222222222222', '0111201122012220',
];
// Anchor (fixed) index of subset 1 for 2-subset partitions
const ANCHOR2 = [
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
  15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
  6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
];
// Anchor index of subset 1 / subset 2 for 3-subset partitions
const ANCHOR3A = [
  3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
  3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
  8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
  3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
];
const ANCHOR3B = [
  15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
  15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
  15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
  15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
];
// flattened: PART2[p*16+i], PART3[p*16+i]
const PART2 = new Uint8Array(64 * 16);
const PART3 = new Uint8Array(64 * 16);
for (let p = 0; p < 64; p++) {
  if (P2[p].length !== 16 || P3[p].length !== 16) throw new Error('BC7 partition table corrupt at ' + p);
  for (let i = 0; i < 16; i++) { PART2[p * 16 + i] = P2[p].charCodeAt(i) - 48; PART3[p * 16 + i] = P3[p].charCodeAt(i) - 48; }
}
const W2 = [0, 21, 43, 64];
const W3 = [0, 9, 18, 27, 37, 46, 55, 64];
const W4 = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];
const WEIGHTS = [null, null, W2, W3, W4];

//            subsets, partBits, rotBits, idxModeBit, colorBits, alphaBits, pbitKind(0 none,1 per-endpoint,2 per-subset), idxBits, idxBits2
const MODES = [
  { ns: 3, pb: 4, rb: 0, im: 0, cb: 4, ab: 0, p: 1, ib: 3, ib2: 0 },
  { ns: 2, pb: 6, rb: 0, im: 0, cb: 6, ab: 0, p: 2, ib: 3, ib2: 0 },
  { ns: 3, pb: 6, rb: 0, im: 0, cb: 5, ab: 0, p: 0, ib: 2, ib2: 0 },
  { ns: 2, pb: 6, rb: 0, im: 0, cb: 7, ab: 0, p: 1, ib: 2, ib2: 0 },
  { ns: 1, pb: 0, rb: 2, im: 1, cb: 5, ab: 6, p: 0, ib: 2, ib2: 3 },
  { ns: 1, pb: 0, rb: 2, im: 0, cb: 7, ab: 8, p: 0, ib: 2, ib2: 2 },
  { ns: 1, pb: 0, rb: 0, im: 0, cb: 7, ab: 7, p: 1, ib: 4, ib2: 0 },
  { ns: 2, pb: 6, rb: 0, im: 0, cb: 5, ab: 5, p: 1, ib: 2, ib2: 0 },
];

// scratch state (module level: no per-block allocations)
const ep = new Int32Array(6 * 4);   // endpoint i, channel c -> ep[i*4+c]
const pbits = new Int32Array(6);
const idxA = new Uint8Array(16);    // primary indices
const idxB = new Uint8Array(16);    // secondary (alpha) indices for modes 4/5
let bp = 0;                         // bit position
let bd = null;                      // block data
let bo = 0;                         // block byte offset

function getBits(n) {
  let v = 0, got = 0;
  while (got < n) {
    const byte = bd[bo + (bp >> 3)];
    const sh = bp & 7;
    const take = Math.min(8 - sh, n - got);
    v |= ((byte >> sh) & ((1 << take) - 1)) << got;
    got += take;
    bp += take;
  }
  return v;
}

/** Decode one 16-byte BC7 block into out[16*4] (RGBA, raster order). */
function decodeBc7Block(data, off, out) {
  bd = data; bo = off; bp = 0;
  let mode = 0;
  while (mode < 8 && !getBits(1)) mode++;
  if (mode >= 8) { out.fill(0); return; } // reserved: transparent black
  const m = MODES[mode];
  const part = m.pb ? getBits(m.pb) : 0;
  const rot = m.rb ? getBits(m.rb) : 0;
  const idxMode = m.im ? getBits(1) : 0;
  const nEp = m.ns * 2;

  for (let c = 0; c < 3; c++) for (let i = 0; i < nEp; i++) ep[i * 4 + c] = getBits(m.cb);
  if (m.ab) for (let i = 0; i < nEp; i++) ep[i * 4 + 3] = getBits(m.ab);
  else for (let i = 0; i < nEp; i++) ep[i * 4 + 3] = 255;

  let extra = 0; // bit depth added by the p-bit
  if (m.p === 1) { for (let i = 0; i < nEp; i++) pbits[i] = getBits(1); extra = 1; }
  else if (m.p === 2) { for (let i = 0; i < m.ns; i++) pbits[i] = getBits(1); extra = 1; }

  // Expand endpoints to 8 bits
  for (let i = 0; i < nEp; i++) {
    const pb = m.p === 1 ? pbits[i] : m.p === 2 ? pbits[i >> 1] : 0;
    for (let c = 0; c < 4; c++) {
      let bits = c < 3 ? m.cb : m.ab;
      if (bits === 0) continue; // opaque alpha stays 255
      let v = ep[i * 4 + c];
      if (extra) { v = (v << 1) | pb; bits++; }
      ep[i * 4 + c] = (v << (8 - bits)) | (v >> (2 * bits - 8));
    }
  }

  // Partition / anchors
  const part16 = part * 16;
  const tab = m.ns === 2 ? PART2 : PART3;
  const a1 = m.ns === 2 ? ANCHOR2[part] : m.ns === 3 ? ANCHOR3A[part] : -1;
  const a2 = m.ns === 3 ? ANCHOR3B[part] : -1;

  // Indices: first set, then (modes 4/5) the second set
  readIndices(idxA, m.ib, m.ns, tab, part16, a1, a2);
  const dual = m.ib2 > 0;
  if (dual) readIndices(idxB, m.ib2, 1, tab, 0, -1, -1);

  const cw = WEIGHTS[m.ib];
  for (let i = 0; i < 16; i++) {
    const s = m.ns === 1 ? 0 : tab[part16 + i];
    const e0 = (s * 2) * 4, e1 = e0 + 4;
    let wc, wa;
    if (!dual) { wc = wa = cw[idxA[i]]; }
    else if (idxMode === 0) { wc = cw[idxA[i]]; wa = WEIGHTS[m.ib2][idxB[i]]; }
    else { wc = WEIGHTS[m.ib2][idxB[i]]; wa = cw[idxA[i]]; }
    let r = (ep[e0] * (64 - wc) + ep[e1] * wc + 32) >> 6;
    let g = (ep[e0 + 1] * (64 - wc) + ep[e1 + 1] * wc + 32) >> 6;
    let b = (ep[e0 + 2] * (64 - wc) + ep[e1 + 2] * wc + 32) >> 6;
    let a = (ep[e0 + 3] * (64 - wa) + ep[e1 + 3] * wa + 32) >> 6;
    if (rot === 1) { const t = a; a = r; r = t; }
    else if (rot === 2) { const t = a; a = g; g = t; }
    else if (rot === 3) { const t = a; a = b; b = t; }
    const o = i * 4;
    out[o] = r; out[o + 1] = g; out[o + 2] = b; out[o + 3] = a;
  }
}

function readIndices(dst, bits, ns, tab, part16, a1, a2) {
  for (let i = 0; i < 16; i++) {
    const s = ns === 1 ? 0 : tab[part16 + i];
    const anchor = i === 0 || (s === 1 && i === a1) || (s === 2 && i === a2);
    dst[i] = getBits(anchor ? bits - 1 : bits);
  }
}

function decodeBc7(data, width, height) {
  const out = new Uint8Array(width * height * 4);
  const px = new Uint8Array(64);
  const bw = (width + 3) >> 2, bh = (height + 3) >> 2;
  for (let by = 0; by < bh; by++) {
    for (let bx = 0; bx < bw; bx++) {
      decodeBc7Block(data, (by * bw + bx) * 16, px);
      blit(out, width, height, bx * 4, by * 4, px);
    }
  }
  return out;
}

function blit(out, width, height, x0, y0, px) {
  const cw = Math.min(4, width - x0), ch = Math.min(4, height - y0);
  for (let y = 0; y < ch; y++) {
    let o = ((y0 + y) * width + x0) * 4;
    for (let x = 0; x < cw; x++, o += 4) {
      const s = (y * 4 + x) * 4;
      out[o] = px[s]; out[o + 1] = px[s + 1]; out[o + 2] = px[s + 2]; out[o + 3] = px[s + 3];
    }
  }
}

// ---- BC1 / BC2 / BC3 ----
const pal = new Uint8Array(16); // 4 colors RGBA

function buildPalette(data, off, force4) {
  const c0 = data[off] | (data[off + 1] << 8);
  const c1 = data[off + 2] | (data[off + 3] << 8);
  const r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
  const r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
  pal[0] = (r0 << 3) | (r0 >> 2); pal[1] = (g0 << 2) | (g0 >> 4); pal[2] = (b0 << 3) | (b0 >> 2); pal[3] = 255;
  pal[4] = (r1 << 3) | (r1 >> 2); pal[5] = (g1 << 2) | (g1 >> 4); pal[6] = (b1 << 3) | (b1 >> 2); pal[7] = 255;
  if (c0 > c1 || force4) {
    for (let c = 0; c < 3; c++) {
      pal[8 + c] = ((2 * pal[c] + pal[4 + c]) / 3) | 0;
      pal[12 + c] = ((pal[c] + 2 * pal[4 + c]) / 3) | 0;
    }
    pal[11] = 255; pal[15] = 255;
  } else {
    for (let c = 0; c < 3; c++) pal[8 + c] = (pal[c] + pal[4 + c]) >> 1;
    pal[11] = 255;
    pal[12] = pal[13] = pal[14] = pal[15] = 0;
  }
}

function colorBlock(data, off, px, force4) {
  buildPalette(data, off, force4);
  const bits = (data[off + 4] | (data[off + 5] << 8) | (data[off + 6] << 16) | (data[off + 7] << 24)) >>> 0;
  for (let i = 0; i < 16; i++) {
    const k = ((bits >>> (i * 2)) & 3) * 4;
    const o = i * 4;
    px[o] = pal[k]; px[o + 1] = pal[k + 1]; px[o + 2] = pal[k + 2]; px[o + 3] = pal[k + 3];
  }
}

const alphaPal = new Uint8Array(8);
function bc3Alpha(data, off, px) {
  const a0 = data[off], a1 = data[off + 1];
  alphaPal[0] = a0; alphaPal[1] = a1;
  if (a0 > a1) for (let i = 1; i < 7; i++) alphaPal[1 + i] = (((7 - i) * a0 + i * a1) / 7) | 0;
  else {
    for (let i = 1; i < 5; i++) alphaPal[1 + i] = (((5 - i) * a0 + i * a1) / 5) | 0;
    alphaPal[6] = 0; alphaPal[7] = 255;
  }
  // 48 bits of 3-bit indices; split into two 24-bit halves
  const lo = data[off + 2] | (data[off + 3] << 8) | (data[off + 4] << 16);
  const hi = data[off + 5] | (data[off + 6] << 8) | (data[off + 7] << 16);
  for (let i = 0; i < 8; i++) {
    px[i * 4 + 3] = alphaPal[(lo >> (i * 3)) & 7];
    px[(i + 8) * 4 + 3] = alphaPal[(hi >> (i * 3)) & 7];
  }
}

function decodeDxt(data, width, height, kind) {
  const out = new Uint8Array(width * height * 4);
  const px = new Uint8Array(64);
  const bw = (width + 3) >> 2, bh = (height + 3) >> 2;
  const stride = kind === 1 ? 8 : 16;
  for (let by = 0; by < bh; by++) {
    for (let bx = 0; bx < bw; bx++) {
      const off = (by * bw + bx) * stride;
      if (kind === 1) colorBlock(data, off, px, false);
      else {
        colorBlock(data, off + 8, px, true);
        if (kind === 3) bc3Alpha(data, off, px);
        else for (let i = 0; i < 16; i++) { // BC2: explicit 4-bit alpha
          const n = (data[off + (i >> 1)] >> ((i & 1) * 4)) & 15;
          px[i * 4 + 3] = (n << 4) | n;
        }
      }
      blit(out, width, height, bx * 4, by * 4, px);
    }
  }
  return out;
}

export function decodeToRgba({ format, width, height, data }) {
  switch (format) {
    case 98: case 99: return decodeBc7(data, width, height);
    case 71: case 72: return decodeDxt(data, width, height, 1);
    case 74: case 75: return decodeDxt(data, width, height, 2);
    case 77: case 78: return decodeDxt(data, width, height, 3);
    case 28: case 29: return new Uint8Array(data.buffer, data.byteOffset, width * height * 4).slice();
    case 87: case 91: {
      const out = new Uint8Array(width * height * 4);
      for (let i = 0; i < out.length; i += 4) {
        out[i] = data[i + 2]; out[i + 1] = data[i + 1]; out[i + 2] = data[i]; out[i + 3] = data[i + 3];
      }
      return out;
    }
    case 61: {
      const out = new Uint8Array(width * height * 4);
      for (let i = 0; i < width * height; i++) { const v = data[i]; out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = v; out[i * 4 + 3] = 255; }
      return out;
    }
    default: throw new Error(`Unsupported texture format (dxgi ${format})`);
  }
}
