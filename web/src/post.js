const KC_ITER = 200000, KC_CHUNK = 1 << 20, KC_HDR = 30, KC_TAG = 16;
const KC_ALPHA = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const post = postOverride || ((m, t) => self.postMessage(m, t || []));

class WrongPassword extends Error { constructor() { super("Falsches Passwort (oder die Datei ist beschädigt)"); this.wrongPassword = true; } }

function progress(phase, done, total) { post({ type: "progress", phase, done, total }); }

function makePassword() {
  const r = crypto.getRandomValues(new Uint8Array(25));
  let s = "";
  for (let i = 0; i < 25; i++) { if (i && i % 5 === 0) s += "-"; s += KC_ALPHA[r[i] & 31]; }
  r.fill(0);
  return s;
}
function normalizePassword(pw) { return String(pw).replace(/[^A-Za-z0-9]/g, "").toUpperCase(); }

function isEncrypted(u8) { return u8.length >= 4 && u8[0] === 0x4B && u8[1] === 0x43 && u8[2] === 0x58 && u8[3] === 0x31; }

async function deriveKey(pw, salt, iter) {
  const base = await crypto.subtle.importKey("raw", new TextEncoder().encode(normalizePassword(pw)), "PBKDF2", false, ["deriveKey"]);
  return crypto.subtle.deriveKey({ name: "PBKDF2", salt, iterations: iter, hash: "SHA-256" }, base,
                                 { name: "AES-GCM", length: 256 }, false, ["encrypt", "decrypt"]);
}
function blockParams(hdr, idx, last) {
  const iv = new Uint8Array(12), aad = new Uint8Array(KC_HDR + 9);
  aad.set(hdr);
  for (let i = 0, v = idx; i < 8; i++, v = Math.floor(v / 256)) { iv[i] = v & 255; aad[KC_HDR + i] = v & 255; }
  aad[KC_HDR + 8] = last ? 1 : 0;
  return { iv, aad };
}

async function encryptKc(plain, pw) {
  const hdr = new Uint8Array(KC_HDR), dv = new DataView(hdr.buffer);
  hdr.set([0x4B, 0x43, 0x58, 0x31, 1, 1]);
  dv.setUint32(6, KC_ITER, true);
  hdr.set(crypto.getRandomValues(new Uint8Array(16)), 10);
  dv.setUint32(26, KC_CHUNK, true);
  progress("Verschlüssele", 0, plain.length);
  const key = await deriveKey(pw, hdr.subarray(10, 26), KC_ITER);
  const n = Math.max(1, Math.ceil(plain.length / KC_CHUNK));
  const out = new Uint8Array(KC_HDR + plain.length + n * KC_TAG);
  out.set(hdr);
  let o = KC_HDR;
  for (let i = 0; i < n; i++) {
    const part = plain.subarray(i * KC_CHUNK, Math.min(plain.length, (i + 1) * KC_CHUNK));
    const { iv, aad } = blockParams(hdr, i, i === n - 1);
    const ct = new Uint8Array(await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: aad, tagLength: 128 }, key, part));
    out.set(ct, o); o += ct.length;
    if (i % 16 === 15) progress("Verschlüssele", (i + 1) * KC_CHUNK, plain.length);
  }
  return out;
}

async function decryptKc(enc, pw) {
  if (enc.length < KC_HDR + KC_TAG || !isEncrypted(enc)) throw new Error("Keine verschlüsselte Klimax-Compression-Datei");
  const hdr = enc.slice(0, KC_HDR), dv = new DataView(hdr.buffer);
  if (hdr[4] !== 1 || hdr[5] !== 1) throw new Error("Verschlüsselung von einer neueren Version. Bitte aktualisieren.");
  const iter = dv.getUint32(6, true), chunk = dv.getUint32(26, true);
  if (iter < 1000 || iter > 10000000 || chunk < 4096 || chunk > (64 << 20)) throw new Error("Archiv beschädigt (Kopf)");
  progress("Entschlüssele", 0, enc.length);
  const key = await deriveKey(pw, hdr.subarray(10, 26), iter);
  let left = enc.length - KC_HDR, pos = KC_HDR, outLen = 0;
  const out = new Uint8Array(left);
  for (let i = 0; left; i++) {
    const rec = Math.min(left, chunk + KC_TAG);
    if (rec < KC_TAG) throw new Error("Archiv beschädigt (abgeschnitten)");
    const { iv, aad } = blockParams(hdr, i, rec === left);
    let pt;
    try { pt = new Uint8Array(await crypto.subtle.decrypt({ name: "AES-GCM", iv, additionalData: aad, tagLength: 128 }, key, enc.subarray(pos, pos + rec))); }
    catch (e) { if (i === 0) throw new WrongPassword(); throw new Error("Archiv beschädigt oder manipuliert (Block " + (i + 1) + ")"); }
    out.set(pt, outLen); outLen += pt.length;
    pos += rec; left -= rec;
    if (i % 16 === 15) progress("Entschlüssele", pos, enc.length);
  }
  return out.subarray(0, outLen);
}

let modPromise = null;
function engine() {
  if (!modPromise) modPromise = KCModule({ onProgress: progress }).then(m => { m._kc_init(); return m; });
  return modPromise;
}

function rmrf(FS, p) {
  let st;
  try { st = FS.stat(p); } catch (e) { return; }
  if (FS.isDir(st.mode)) {
    for (const n of FS.readdir(p)) if (n !== "." && n !== "..") rmrf(FS, p + "/" + n);
    FS.rmdir(p);
  } else FS.unlink(p);
}
function mkdirp(FS, p) {
  let cur = "";
  for (const part of p.split("/").filter(Boolean)) {
    cur += "/" + part;
    try { FS.mkdir(cur); } catch (e) {}
  }
}
function walk(FS, root, rel, out) {
  const kids = FS.readdir(rel ? root + "/" + rel : root).filter(n => n !== "." && n !== "..").map(n => {
    const p = rel ? rel + "/" + n : n, st = FS.stat(root + "/" + p);
    return { n, p, dir: FS.isDir(st.mode), size: st.size };
  });
  kids.sort((a, b) => (b.dir - a.dir) || a.n.localeCompare(b.n, "de", { numeric: true, sensitivity: "base" }));
  for (const k of kids) {
    if (k.dir) { out.push({ path: k.p, dir: true, size: 0 }); walk(FS, root, k.p, out); }
    else out.push({ path: k.p, dir: false, size: k.size });
  }
  return out;
}
function safeRel(p) {
  if (!p || p.length > 2000 || p[0] === "/") return false;
  return p.split("/").every(c => c && c !== "." && c !== ".." && !/[\\:*?"<>|\x00-\x1f]/.test(c));
}

let CRC_T = null;
function crc32(u8) {
  if (!CRC_T) { CRC_T = new Uint32Array(256); for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; CRC_T[i] = c >>> 0; } }
  let c = 0xFFFFFFFF;
  for (let i = 0; i < u8.length; i++) c = CRC_T[(c ^ u8[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}
function buildZip(FS, root, items) {
  const enc = new TextEncoder();
  const now = new Date();
  const dosTime = (now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1);
  const dosDate = ((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate();
  const locals = [], centrals = [];
  let offset = 0, total = 0;
  for (const it of items) total += it.size;
  if (total > 0xF0000000 || items.length > 65000) throw new Error("Zu groß für eine ZIP-Datei im Browser.");
  let done = 0;
  for (const it of items) {
    const name = enc.encode(it.dir ? it.path + "/" : it.path);
    const data = it.dir ? new Uint8Array(0) : FS.readFile(root + "/" + it.path);
    const crc = crc32(data);
    const lh = new Uint8Array(30 + name.length), lv = new DataView(lh.buffer);
    lv.setUint32(0, 0x04034b50, true); lv.setUint16(4, 20, true); lv.setUint16(6, 0x0800, true); lv.setUint16(8, 0, true);
    lv.setUint16(10, dosTime, true); lv.setUint16(12, dosDate, true); lv.setUint32(14, crc, true);
    lv.setUint32(18, data.length, true); lv.setUint32(22, data.length, true); lv.setUint16(26, name.length, true); lv.setUint16(28, 0, true);
    lh.set(name, 30);
    const ch = new Uint8Array(46 + name.length), cv = new DataView(ch.buffer);
    cv.setUint32(0, 0x02014b50, true); cv.setUint16(4, 20, true); cv.setUint16(6, 20, true); cv.setUint16(8, 0x0800, true);
    cv.setUint16(10, 0, true); cv.setUint16(12, dosTime, true); cv.setUint16(14, dosDate, true); cv.setUint32(16, crc, true);
    cv.setUint32(20, data.length, true); cv.setUint32(24, data.length, true); cv.setUint16(28, name.length, true);
    cv.setUint32(38, it.dir ? 0x10 : 0, true); cv.setUint32(42, offset, true);
    ch.set(name, 46);
    locals.push(lh, data); centrals.push(ch);
    offset += lh.length + data.length;
    done += it.size;
    progress("ZIP wird gebaut", done, total);
  }
  const cdSize = centrals.reduce((a, c) => a + c.length, 0);
  const end = new Uint8Array(22), ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, items.length, true); ev.setUint16(10, items.length, true);
  ev.setUint32(12, cdSize, true); ev.setUint32(16, offset, true);
  const out = new Uint8Array(offset + cdSize + 22);
  let o = 0;
  for (const part of locals) { out.set(part, o); o += part.length; }
  for (const part of centrals) { out.set(part, o); o += part.length; }
  out.set(end, o);
  return out;
}

async function doPack(msg) {
  const M = await engine(), FS = M.FS;
  rmrf(FS, "/w"); mkdirp(FS, "/w/in"); mkdirp(FS, "/w/out");
  const total = msg.files.reduce((a, f) => a + f.file.size, 0);
  let read = 0;
  progress("Lese Dateien", 0, total);
  for (const { path, file } of msg.files) {
    if (!safeRel(path)) throw new Error("Ungültiger Pfad: " + path);
    const i = path.lastIndexOf("/");
    if (i > 0) mkdirp(FS, "/w/in/" + path.slice(0, i));
    FS.writeFile("/w/in/" + path, new Uint8Array(await file.arrayBuffer()));
    if (file.lastModified) { try { FS.utime("/w/in/" + path, file.lastModified, file.lastModified); } catch (e) {} }
    read += file.size;
    progress("Lese Dateien", read, total);
  }
  for (const d of msg.dirs || []) if (safeRel(d)) mkdirp(FS, "/w/in/" + d);
  const top = FS.readdir("/w/in").filter(n => n !== "." && n !== "..");
  if (top.length !== 1) throw new Error("Interner Fehler: genau ein Wurzelelement erwartet.");
  const name = top[0] + ".kc", out = "/w/out/" + name;
  const t0 = performance.now();
  const rc = M.ccall("kc_compress", "number", ["string", "string", "number"], ["/w/in/" + top[0], out, msg.mode | 0]);
  if (rc) { const err = M.UTF8ToString(M._kc_error()); rmrf(FS, "/w"); throw new Error(err); }
  const info = JSON.parse(M.UTF8ToString(M._kc_info()));
  rmrf(FS, "/w/in");
  let data = FS.readFile(out);
  rmrf(FS, "/w");
  let password = "";
  if (msg.secret) { password = makePassword(); data = await encryptKc(data, password); }
  info.outBytes = data.length;
  info.seconds = (performance.now() - t0) / 1000;
  post({ type: "packed", name, info, password, data: data.buffer }, [data.buffer]);
}

let opened = null;
async function doOpen(msg) {
  const M = await engine(), FS = M.FS;
  let data = new Uint8Array(await msg.file.arrayBuffer());
  const encrypted = isEncrypted(data);
  if (encrypted) {
    if (!msg.password) { post({ type: "needPassword", wrong: false }); return; }
    try { data = await decryptKc(data, msg.password); }
    catch (e) { if (e.wrongPassword) { post({ type: "needPassword", wrong: true }); return; } throw e; }
  }
  rmrf(FS, "/x"); mkdirp(FS, "/x/out");
  FS.writeFile("/x/a.kc", data);
  data = null;
  if (msg.maxBytes) {
    if (M.ccall("kc_peek", "number", ["string"], ["/x/a.kc"])) { const err = M.UTF8ToString(M._kc_error()); rmrf(FS, "/x"); throw new Error(err); }
    const peek = JSON.parse(M.UTF8ToString(M._kc_info()));
    if (peek.totalSize > msg.maxBytes) { rmrf(FS, "/x"); post({ type: "tooBig", total: peek.totalSize }); return; }
  }
  const t0 = performance.now();
  const rc = M.ccall("kc_extract", "number", ["string", "string"], ["/x/a.kc", "/x/out"]);
  try { FS.unlink("/x/a.kc"); } catch (e) {}
  if (rc) { const err = M.UTF8ToString(M._kc_error()); rmrf(FS, "/x"); throw new Error(err); }
  const info = JSON.parse(M.UTF8ToString(M._kc_info()));
  info.encrypted = encrypted;
  info.seconds = (performance.now() - t0) / 1000;
  opened = walk(FS, "/x/out", "", []);
  post({ type: "opened", list: opened, info });
}

async function doGet(msg) {
  const M = await engine();
  if (!opened || !safeRel(msg.path)) throw new Error("Datei nicht gefunden.");
  const data = M.FS.readFile("/x/out/" + msg.path);
  post({ type: "file", path: msg.path, data: data.buffer, tag: msg.tag }, [data.buffer]);
}

async function doZip(msg) {
  const M = await engine();
  if (!opened) throw new Error("Kein Archiv geöffnet.");
  const zip = buildZip(M.FS, "/x/out", opened);
  post({ type: "zip", data: zip.buffer }, [zip.buffer]);
}

async function handle(msg) {
  try {
    if (msg.cmd === "pack") await doPack(msg);
    else if (msg.cmd === "open") await doOpen(msg);
    else if (msg.cmd === "get") await doGet(msg);
    else if (msg.cmd === "zip") await doZip(msg);
    else if (msg.cmd === "warmup") { await engine(); post({ type: "ready" }); }
  } catch (e) {
    post({ type: "error", message: (e && e.message) || (e && e.name === "ErrnoError" ? "Dateisystemfehler im Browser (" + e.errno + ")" : String(e)) });
  }
}

if (!postOverride && typeof self !== "undefined" && typeof self.importScripts === "function") self.onmessage = e => handle(e.data);
return { handle, makePassword, encryptKc, decryptKc, normalizePassword };
}
