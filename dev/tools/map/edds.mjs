// EDDS (Enfusion DDS) parser. Layout: 'DDS ' header (128 B) [+ DX10 header (20 B)] + mip table of
// N x {fourcc 'COPY'|'LZ4 ', u32 size} (smallest mip first) + mip bodies in the same order.
// LZ4 body: u32 total size, then chunks [u32 (flag<<24)|compressedSize][LZ4 block]; chunks are LINKED
// (matches may reach into earlier chunks' output), so everything decodes into one contiguous buffer.
// Plain DDS (no mip table, largest mip first) is handled too.
//   parseEdds(buf) -> {width, height, format (dxgi), mips:[{width,height,data}]} largest first

export const DXGI = {
  R8G8B8A8_UNORM: 28, R8G8B8A8_UNORM_SRGB: 29, R8_UNORM: 61,
  BC1_UNORM: 71, BC1_UNORM_SRGB: 72, BC2_UNORM: 74, BC2_UNORM_SRGB: 75,
  BC3_UNORM: 77, BC3_UNORM_SRGB: 78, BC7_UNORM: 98, BC7_UNORM_SRGB: 99,
  B8G8R8A8_UNORM: 87, B8G8R8A8_UNORM_SRGB: 91,
};

const BLOCK_BYTES = { 71: 8, 72: 8, 74: 16, 75: 16, 77: 16, 78: 16, 80: 8, 81: 8, 83: 16, 84: 16, 98: 16, 99: 16 };

export function mipByteSize(format, w, h) {
  const bb = BLOCK_BYTES[format];
  if (bb) return Math.ceil(w / 4) * Math.ceil(h / 4) * bb;
  if (format === DXGI.R8_UNORM) return w * h;
  return w * h * 4;
}

/** Decode one LZ4 block from src[sp..se) into dst at dp (dst also serves as match history). Returns new dp. */
export function lz4Block(src, sp, se, dst, dp) {
  const dEnd = dst.length;
  while (sp < se) {
    const token = src[sp++];
    let lit = token >> 4;
    if (lit === 15) {
      let b;
      do { b = src[sp++]; lit += b; } while (b === 255 && sp < se);
    }
    if (dp + lit > dEnd || sp + lit > se) throw new Error('LZ4: literal run out of bounds');
    for (let i = 0; i < lit; i++) dst[dp++] = src[sp++];
    if (sp >= se) break; // last sequence has literals only
    const off = src[sp] | (src[sp + 1] << 8);
    sp += 2;
    let ml = token & 15;
    if (ml === 15) {
      let b;
      do { b = src[sp++]; ml += b; } while (b === 255 && sp < se);
    }
    ml += 4;
    if (off === 0 || off > dp) throw new Error('LZ4: bad match offset');
    if (dp + ml > dEnd) throw new Error('LZ4: match out of bounds');
    let from = dp - off;
    for (let i = 0; i < ml; i++) dst[dp++] = dst[from++]; // byte loop: overlapping copies are intended
  }
  return dp;
}

function decodeLz4Body(body) {
  const total = body.readUInt32LE(0);
  const out = Buffer.alloc(total);
  let sp = 4, dp = 0;
  while (sp + 4 <= body.length && dp < total) {
    const hdr = body.readUInt32LE(sp);
    sp += 4;
    const csize = hdr & 0x00ffffff;
    if (sp + csize > body.length) throw new Error('EDDS: LZ4 chunk exceeds body');
    // Chunk output is capped at 64 KiB by the writer; use a bounded view of the shared buffer is not
    // possible (history needed), so decode against the full buffer and rely on block termination.
    dp = lz4Block(body, sp, sp + csize, out, dp);
    sp += csize;
    if (hdr >>> 24 & 0x80) break;
  }
  if (dp !== total) throw new Error(`EDDS: LZ4 produced ${dp} of ${total} bytes`);
  return out;
}

export function parseEdds(buf) {
  if (buf.length < 128 || buf.toString('latin1', 0, 4) !== 'DDS ') throw new Error('Not a DDS/EDDS file');
  const height = buf.readUInt32LE(12);
  const width = buf.readUInt32LE(16);
  const mipCount = Math.max(1, buf.readUInt32LE(28));
  const pfFlags = buf.readUInt32LE(80);
  const fourcc = buf.toString('latin1', 84, 88);
  let pos = 128;
  let format;
  if (fourcc === 'DX10') {
    format = buf.readUInt32LE(128);
    pos = 148;
  } else if (pfFlags & 4) { // DDPF_FOURCC
    format = { DXT1: 71, DXT3: 74, DXT5: 77, ATI1: 80, ATI2: 83, BC4U: 80, BC5U: 83 }[fourcc];
    if (!format) throw new Error(`Unsupported DDS fourcc '${fourcc}'`);
  } else if (pfFlags & 0x40) { // DDPF_RGB, assume 32 bpp
    const bits = buf.readUInt32LE(88);
    if (bits !== 32) throw new Error(`Unsupported DDS RGB bit count ${bits}`);
    format = buf.readUInt32LE(92) === 0xff ? DXGI.R8G8B8A8_UNORM : DXGI.B8G8R8A8_UNORM; // red mask
  } else {
    throw new Error('Unsupported DDS pixel format');
  }

  const dims = [];
  for (let l = 0; l < mipCount; l++) dims.push([Math.max(1, width >> l), Math.max(1, height >> l)]);
  const mips = new Array(mipCount);

  const tag = buf.toString('latin1', pos, pos + 4);
  if (tag === 'COPY' || tag === 'LZ4 ') {
    let bodyPos = pos + 8 * mipCount;
    for (let i = 0; i < mipCount; i++) {
      const kind = buf.toString('latin1', pos + 8 * i, pos + 8 * i + 4);
      const size = buf.readUInt32LE(pos + 8 * i + 4);
      const level = mipCount - 1 - i;
      const body = buf.subarray(bodyPos, bodyPos + size);
      bodyPos += size;
      const [w, h] = dims[level];
      let data;
      if (kind === 'COPY') data = body;
      else if (kind === 'LZ4 ') data = decodeLz4Body(body);
      else throw new Error(`EDDS: unknown mip encoding '${kind}'`);
      mips[level] = { width: w, height: h, data };
    }
  } else {
    for (let l = 0; l < mipCount; l++) {
      const [w, h] = dims[l];
      const size = mipByteSize(format, w, h);
      if (pos + size > buf.length) throw new Error('DDS truncated');
      mips[l] = { width: w, height: h, data: buf.subarray(pos, pos + size) };
      pos += size;
    }
  }
  return { width, height, format, mips };
}
