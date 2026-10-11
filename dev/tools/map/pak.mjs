// Enfusion .pak reader (zero dependencies). Port of enfusion-workbench-mcp src/pak/reader.ts + vfs.ts
// without the 8 MiB cap. Only the FILE tree is parsed up front; entry data is read by offset via fs handles.
//
//   const pak = openPak(path);          // lazy + cached index
//   readEntry(pak, 'UI/Textures/x.edds') // -> Buffer (case-insensitive, forward slashes)
//   listEntries(pak)                     // -> [{path, size}]
import { openSync, readSync, closeSync, fstatSync } from 'node:fs';
import { inflateSync, inflateRawSync } from 'node:zlib';

const MAGIC_FORM = 0x464f524d, MAGIC_PAC1 = 0x50414331;
const MAGIC_HEAD = 0x48454144, MAGIC_DATA = 0x44415441, MAGIC_FILE = 0x46494c45;
const MAX_DEPTH = 64;

const cache = new Map();

export function normalizeEntryPath(p) {
  return String(p).replace(/\\/g, '/').replace(/^\/+|\/+$/g, '').replace(/\/+/g, '/').toLowerCase();
}

function readAt(fd, pos, len) {
  const buf = Buffer.alloc(len);
  const n = readSync(fd, buf, 0, len, pos);
  if (n < len) throw new Error(`Unexpected EOF: wanted ${len} bytes at ${pos}, got ${n}`);
  return buf;
}

/** Open (parse index of) a pak file. Cached per absolute path. Throws on non-pak files. */
export function openPak(pakPath) {
  let pak = cache.get(pakPath);
  if (pak) return pak;
  pak = parsePak(pakPath);
  cache.set(pakPath, pak);
  return pak;
}

function parsePak(pakPath) {
  const fd = openSync(pakPath, 'r');
  try {
    const fileSize = fstatSync(fd).size;
    const form = readAt(fd, 0, 12);
    if (form.readUInt32BE(0) !== MAGIC_FORM) throw new Error(`Not a PAK file (no FORM): ${pakPath}`);
    if (form.readUInt32BE(8) !== MAGIC_PAC1) throw new Error(`Not a PAK file (no PAC1): ${pakPath}`);

    let pos = 12, dataStart = -1, fileOff = -1, fileLen = 0, headVersion = 0;
    while (pos + 8 <= fileSize) {
      const hdr = readAt(fd, pos, 8);
      const magic = hdr.readUInt32BE(0), len = hdr.readUInt32BE(4);
      if (len > fileSize - pos - 8) throw new Error(`PAK chunk at ${pos} overruns file: ${pakPath}`);
      if (magic === MAGIC_HEAD) {
        if (len >= 2) headVersion = readAt(fd, pos + 8, 2).readUInt16LE(0);
        pos += 8 + len;
      } else if (magic === MAGIC_DATA) {
        dataStart = pos + 8;
        pos += 8 + len;
      } else if (magic === MAGIC_FILE) {
        fileOff = pos + 8; fileLen = len;
        break;
      } else {
        pos += 8 + len;
      }
    }
    if (dataStart < 0 || fileOff < 0) throw new Error(`PAK missing DATA/FILE chunk: ${pakPath}`);

    const buf = readAt(fd, fileOff, fileLen);
    const files = new Map(); // lowercase path -> entry
    const st = { o: 0 };
    parseEntry(buf, st, 0, '', files);
    return { path: pakPath, dataStart, headVersion, files, verified: null };
  } finally {
    closeSync(fd);
  }
}

function need(buf, o, n) {
  if (o + n > buf.length) throw new Error('PAK FILE chunk truncated');
}

function parseEntry(buf, st, depth, prefix, files) {
  if (depth > MAX_DEPTH) throw new Error('PAK FILE tree too deep');
  need(buf, st.o, 2);
  const kind = buf[st.o++];
  const nameLen = buf[st.o++];
  need(buf, st.o, nameLen);
  const name = buf.toString('utf8', st.o, st.o + nameLen);
  st.o += nameLen;
  const path = prefix && name ? `${prefix}/${name}` : (prefix || name);
  if (kind === 0) {
    need(buf, st.o, 4);
    const n = buf.readUInt32LE(st.o);
    st.o += 4;
    for (let i = 0; i < n; i++) parseEntry(buf, st, depth + 1, path, files);
  } else {
    // offset, compressedLen, size (u32 each), u32 + u16 unknown, u8 compressed, u8 level, u32 timestamp = 24 bytes
    need(buf, st.o, 24);
    const offset = buf.readUInt32LE(st.o);
    const compressedLen = buf.readUInt32LE(st.o + 4);
    const size = buf.readUInt32LE(st.o + 8);
    const compressed = buf[st.o + 18] !== 0;
    st.o += 24;
    const key = path.toLowerCase();
    if (!files.has(key)) files.set(key, { path, offset, compressedLen, size, compressed });
  }
}

export function hasEntry(pak, entryPath) {
  return pak.files.has(normalizeEntryPath(entryPath));
}

export function listEntries(pak) {
  return Array.from(pak.files.values(), (e) => ({ path: e.path, size: e.size }));
}

/** RFC 1950 zlib header sniff (1.8+ paks are zlib wrapped, older ones raw deflate). */
function looksZlib(b) {
  return b.length >= 2 && (b[0] & 0x0f) === 8 && ((b[0] << 8) | b[1]) % 31 === 0;
}

/** Read + decompress one entry. Throws if missing/corrupt. */
export function readEntry(pak, entryPath) {
  const e = pak.files.get(normalizeEntryPath(entryPath));
  if (!e) throw new Error(`Entry not found in ${pak.path}: ${entryPath}`);
  const readLen = e.compressed ? e.compressedLen : e.size;
  const fd = openSync(pak.path, 'r');
  try {
    const pakSize = fstatSync(fd).size;
    // v3 (game 1.8+) paks store absolute offsets; older ones are relative to the DATA payload.
    const preferAbs = pak.verified ? pak.verified.abs : pak.headVersion >= 3;
    const cands = preferAbs ? [e.offset, pak.dataStart + e.offset] : [pak.dataStart + e.offset, e.offset];
    let lastErr = null;
    for (const pos of cands) {
      if (pos < 12 || pos + readLen > pakSize) {
        lastErr = new Error(`read window [${pos}, ${pos + readLen}) outside pak (size ${pakSize})`);
        continue;
      }
      const buf = Buffer.alloc(readLen);
      if (readLen > 0 && readSync(fd, buf, 0, readLen, pos) < readLen) {
        lastErr = new Error('truncated read');
        continue;
      }
      if (!e.compressed) return buf; // no signature to verify a stored entry against
      try {
        const opts = { maxOutputLength: Math.max(e.size, 1) };
        const out = looksZlib(buf) ? inflateSync(buf, opts) : inflateRawSync(buf, opts);
        if (out.length !== e.size) throw new Error(`inflated ${out.length} bytes, header says ${e.size}`);
        pak.verified = { abs: pos === e.offset };
        return out;
      } catch (err) {
        lastErr = err;
      }
    }
    throw new Error(`Failed to read ${entryPath} from ${pak.path}: ${lastErr ? lastErr.message : 'unknown'}`);
  } finally {
    closeSync(fd);
  }
}
