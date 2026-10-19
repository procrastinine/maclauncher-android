const fs = require("node:fs");
const zlib = require("node:zlib");
const crypto = require("node:crypto");
const { Readable, Transform } = require("node:stream");
const { pipeline } = require("node:stream/promises");
const Zip = require("../shared/runtime/zip");
const Manifest = require("./binary-manifest");
const LIMITS = Object.freeze({ directoryBytes: 16 * 1024 * 1024, entries: 65534, fileBytes: 256 * 1024 * 1024,
  extractedBytes: 4 * 1024 ** 3, chunkBytes: 64 * 1024 });

function withApk(file, callback) {
  const fd = fs.openSync(file, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW);
  let deferred = false;
  try {
    const before = fs.fstatSync(fd);
    if (!before.isFile()) throw new Error("Choose an APK file.");
    const info = Zip.findZipEocd(fd);
    if (!info || info.zipBase !== 0 || !info.entries || info.entries > LIMITS.entries ||
      !info.cdSize || info.cdSize > LIMITS.directoryBytes || info.cdOffset + info.cdSize !== info.eocdOffset)
      throw new Error("The APK archive is incomplete or unsupported.");
    const entries = Zip.readCentralDirectory(fd, info, { metadata: true });
    if (entries.length !== info.entries) throw new Error("The APK directory is incomplete.");
    const names = new Map();
    for (const entry of entries) {
      const name = entry.name;
      if (!name || name.startsWith("/") || /[\\\x00-\x1f:]/.test(name) ||
        name.replace(/\/$/, "").split("/").some(part => !part || part === "." || part === "..") || names.has(name))
        throw new Error("The APK contains ambiguous or invalid file paths.");
      names.set(name, entry);
    }
    function readAt(at, length) {
      if (!Number.isSafeInteger(at) || at < 0 || length < 0 || at > info.cdOffset - length) throw new Error("The APK entry exceeds its file range.");
      const buffer = Buffer.alloc(length);
      if (fs.readSync(fd, buffer, 0, length, at) !== length) throw new Error("The APK entry is truncated.");
      return buffer;
    }
    function locate(name, maximum) {
      const entry = names.get(name);
      if (!entry) return null;
      if (entry.uncompressedSize > maximum || entry.compressedSize > maximum + 65536) throw new Error("The APK entry exceeds its size limit.");
      const header = readAt(entry.absoluteLocalHeaderOffset, 30);
      if (header.readUInt32LE(0) !== 0x04034b50 || (header.readUInt16LE(6) & 1) ||
        header.readUInt16LE(6) !== entry.flags || header.readUInt16LE(8) !== entry.compressionMethod)
        throw new Error("Encrypted or invalid APK entries are unsupported.");
      const nameLength = header.readUInt16LE(26), extraLength = header.readUInt16LE(28);
      const declaredName = readAt(entry.absoluteLocalHeaderOffset + 30, nameLength).toString("utf8");
      if (declaredName !== name) throw new Error("The APK entry name does not match its directory.");
      const method = header.readUInt16LE(8), offset = entry.absoluteLocalHeaderOffset + 30 + nameLength + extraLength;
      if (![0, 8].includes(method)) throw new Error("The APK entry cannot be decoded.");
      if (offset > info.cdOffset - entry.compressedSize) throw new Error("The APK entry exceeds its file range.");
      if (!(entry.flags & 8) && entry.crc32 !== header.readUInt32LE(14)) throw new Error("The APK entry checksum does not match.");
      return { entry, method, offset };
    }
    function decode(location, maximum) {
      const { entry, method, offset } = location, compressed = readAt(offset, entry.compressedSize);
      let data;
      try { data = method === 0 ? compressed : zlib.inflateRawSync(compressed, { maxOutputLength: maximum }); }
      catch(error) { if(error.code === "ERR_BUFFER_TOO_LARGE") throw new Error("The APK entry exceeds its size limit."); throw error; }
      if (!data || data.length !== entry.uncompressedSize) throw new Error("The APK entry cannot be decoded.");
      if (zlib.crc32(data) !== entry.crc32) throw new Error("The APK entry checksum does not match.");
      return data;
    }
    function read(name, maximum = LIMITS.fileBytes) {
      if (!Number.isSafeInteger(maximum) || maximum < 1 || maximum > LIMITS.fileBytes) throw new Error("Invalid APK read limit.");
      const location = locate(name, maximum);
      return location ? decode(location, maximum) : null;
    }
    // Await inside the withApk callback so the original descriptor stays open.
    // Large game assets are decoded directly to disk, with bounded buffers.
    async function copy(name, output, { maximum = LIMITS.extractedBytes, signal } = {}) {
      if (!Number.isSafeInteger(maximum) || maximum < 1 || maximum > LIMITS.extractedBytes) throw new Error("Invalid APK extraction limit.");
      signal?.throwIfAborted();
      const location = locate(name, maximum);
      if (!location) throw new Error("The APK entry is missing.");
      const { entry, method, offset } = location;
      // Small assets fit the existing bounded chunk. Avoid constructing four
      // streams for each one; asynchronous file operations still yield between
      // entries, and larger assets retain the streaming path below.
      if (entry.uncompressedSize <= LIMITS.chunkBytes && entry.compressedSize <= LIMITS.chunkBytes) {
        const data = decode(location, Math.max(1, entry.uncompressedSize));
        signal?.throwIfAborted();
        const target = await fs.promises.open(output, "wx", 0o600);
        let complete = false;
        try {
          signal?.throwIfAborted();
          await target.writeFile(data);
          signal?.throwIfAborted();
          complete = true;
          return { bytes: data.length, sha256: crypto.createHash("sha256").update(data).digest("hex") };
        } finally {
          await target.close();
          if (!complete) await fs.promises.rm(output, { force: true });
        }
      }
      async function* chunks() {
        for (let position = 0; position < entry.compressedSize;) {
          signal?.throwIfAborted();
          const buffer = Buffer.allocUnsafe(Math.min(LIMITS.chunkBytes, entry.compressedSize - position));
          const count = await new Promise((resolve, reject) => fs.read(fd, buffer, 0, buffer.length, offset + position,
            (error, bytesRead) => error ? reject(error) : resolve(bytesRead)));
          if (!count) throw new Error("The APK entry is truncated.");
          position += count; yield count === buffer.length ? buffer : buffer.subarray(0, count);
        }
      }
      let bytes = 0, crc = 0, complete = false;
      const digest = crypto.createHash("sha256");
      const meter = new Transform({ transform(chunk, _encoding, done) {
        bytes += chunk.length;
        if (bytes > entry.uncompressedSize || bytes > maximum) return done(new Error("The APK entry exceeds its size limit."));
        crc = zlib.crc32(chunk, crc); digest.update(chunk); done(null, chunk);
      } });
      const target = await fs.promises.open(output, "wx", 0o600);
      try {
        const streams = [Readable.from(chunks(), { objectMode: false })];
        if (method === 8) streams.push(zlib.createInflateRaw());
        await pipeline(...streams, meter, target.createWriteStream(), { signal });
        if (bytes !== entry.uncompressedSize) throw new Error("The APK entry cannot be decoded.");
        if (crc !== entry.crc32) throw new Error("The APK entry checksum does not match.");
        complete = true;
        return { bytes, sha256: digest.digest("hex") };
      } finally {
        await target.close();
        if (!complete) await fs.promises.rm(output, { force: true });
      }
    }
    const unchanged = () => {
      const after = fs.fstatSync(fd);
      if (after.size !== before.size || after.mtimeMs !== before.mtimeMs || after.ctimeMs !== before.ctimeMs) throw new Error("The APK changed during inspection.");
    };
    const result = callback({ entries, read, copy, stat: before });
    if (result && typeof result.then === "function") {
      deferred = true;
      return Promise.resolve(result).then(value => { unchanged(); return value; }).finally(() => fs.closeSync(fd));
    }
    unchanged();
    return result;
  } finally { if (!deferred) fs.closeSync(fd); }
}

function inspectApk(file, { requireLauncher = true, nativeArchitectures = ["arm64-v8a"] } = {}) {
  return withApk(file, ({ entries, read }) => {
    const manifest = Manifest.parseBinaryManifest(read("AndroidManifest.xml", Manifest.LIMITS.bytes));
    const dex = entries.filter(entry => /^classes(?:[2-9]|[1-9]\d+)?\.dex$/.test(entry.name)).map(entry => entry.name).sort();
    const libraries = entries.filter(entry => /^lib\/[^/]+\/[^/]+\.so$/.test(entry.name)).map(entry => entry.name);
    const architectures = [...new Set(libraries.map(name => name.split("/")[1]))].sort();
    const limitations = [];
    if (manifest.split || manifest.requiredSplits.length || manifest.requiresSplitInstallation) limitations.push("Additional split APKs are required. Select the base APK with its matching splits or a complete XAPK, APKM or APKS archive.");
    if (requireLauncher && !manifest.launchers.length) limitations.push("The APK has no enabled launcher activity.");
    if (architectures.length && !nativeArchitectures.some(abi => architectures.includes(abi))) limitations.push("The APK does not include native libraries compatible with the installed Android runtime.");
    // Resource packages and framework-backed applications need no local DEX.
    // Original class loading resolves any declared classes from their actual
    // framework/shared-library parents; an activity name is not proof of code.
    if (!dex.length && manifest.hasCode !== false) limitations.push("The APK is missing application bytecode required by its manifest.");
    return { ...manifest, dex, architectures, libraries, limitations };
  });
}

module.exports = { LIMITS, withApk, inspectApk };
