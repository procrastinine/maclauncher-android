const ANDROID = "http://schemas.android.com/apk/res/android", NONE = 0xffffffff;
const node = (name, attrs = {}, children = []) => ({ name, attrs, children });
const activity = (name = ".Main", extra = {}) => node("activity", { name, ...extra }, [node("intent-filter", {}, [
  node("action", { name: "android.intent.action.MAIN" }), node("category", { name: "android.intent.category.LAUNCHER" })])]);
function manifest(appChildren = [activity()], app = {}, root = {}) {
  return node("manifest", { ":package": "org.example.game", versionName: "1.2.3", versionCode: 7, ...root }, [
    node("uses-sdk", { minSdkVersion: 21, targetSdkVersion: 36 }), node("uses-permission", { name: "android.permission.INTERNET" }),
    node("application", { label: "Example ゲーム", ...app }, appChildren)]);
}
function binaryXml(tree, utf8 = true, resourceIds = null) {
  const strings = [], index = value => { if (!strings.includes(value)) strings.push(value); return strings.indexOf(value); };
  if (resourceIds) {
    for (const name of Object.keys(resourceIds).sort((a,b)=>resourceIds[a]-resourceIds[b])) index(name);
    index('android'); index(ANDROID);
  }
  function visit(item) { index(item.name); if (item.namespace) index(item.namespace); if (item.endNamespace) index(item.endNamespace); for (const [key, value] of Object.entries(item.attrs)) {
    if (!key.startsWith(":")) index(ANDROID); index(key.replace(/^:/, "")); if (typeof value === "string") index(value);
  } item.children.forEach(visit); }
  visit(tree);
  function chunk(type, headerSize, body) { const head = Buffer.alloc(headerSize); head.writeUInt16LE(type); head.writeUInt16LE(headerSize, 2); head.writeUInt32LE(headerSize + body.length, 4); return Buffer.concat([head, body]); }
  function length8(size) { return size > 127 ? Buffer.from([0x80 | size >> 8, size & 255]) : Buffer.from([size]); }
  const pieces = strings.map(value => { const data = Buffer.from(value, utf8 ? "utf8" : "utf16le");
    if (utf8) return Buffer.concat([length8(value.length), length8(data.length), data, Buffer.alloc(1)]);
    const size = Buffer.alloc(2); size.writeUInt16LE(value.length); return Buffer.concat([size, data, Buffer.alloc(2)]);
  });
  const table = Buffer.alloc(strings.length * 4); let offset = 0;
  pieces.forEach((piece, i) => { table.writeUInt32LE(offset, i * 4); offset += piece.length; });
  const padding = resourceIds ? Buffer.alloc((4 - (28 + table.length + offset) % 4) % 4) : Buffer.alloc(0);
  const pool = chunk(1, 28, Buffer.concat([table, ...pieces, padding])); pool.writeUInt32LE(strings.length, 8);
  pool.writeUInt32LE(utf8 ? 0x100 : 0, 16); pool.writeUInt32LE(28 + table.length, 20);
  const xml = [pool];
  function namespace(kind) {
    const body = Buffer.alloc(8); body.writeUInt32LE(index('android')); body.writeUInt32LE(index(ANDROID),4);
    return chunk(kind,16,body);
  }
  if (resourceIds) {
    const map = Buffer.alloc(strings.length*4);
    for (const [name,id] of Object.entries(resourceIds)) map.writeUInt32LE(id,index(name)*4);
    xml.push(chunk(0x180,8,map),namespace(0x100));
  }
  function emit(item) {
    const ext = Buffer.alloc(20), attrs = Object.entries(item.attrs);
    if (resourceIds) attrs.sort(([a],[b])=>(a.startsWith(':')?0:resourceIds[a]||0)-(b.startsWith(':')?0:resourceIds[b]||0));
    ext.writeUInt32LE(item.namespace ? index(item.namespace) : NONE); ext.writeUInt32LE(index(item.name), 4); ext.writeUInt16LE(20, 8); ext.writeUInt16LE(20, 10); ext.writeUInt16LE(attrs.length, 12);
    const data = attrs.map(([key, value]) => { const a = Buffer.alloc(20); a.writeUInt32LE(key.startsWith(":") ? NONE : index(ANDROID));
      a.writeUInt32LE(index(key.replace(/^:/, "")), 4); a.writeUInt32LE(typeof value === "string" ? index(value) : NONE, 8); a.writeUInt16LE(8, 12);
      a[15] = typeof value === "object" ? value.type : typeof value === "string" ? 3 : typeof value === "boolean" ? 0x12 : 0x10;
      a.writeUInt32LE(typeof value === "object" ? value.data : typeof value === "string" ? index(value) : typeof value === "boolean" ? Number(value) : value, 16); return a;
    });
    xml.push(chunk(0x102, 16, Buffer.concat([ext, ...data]))); item.children.forEach(emit);
    const end = Buffer.alloc(8), endNamespace = item.endNamespace ?? item.namespace;
    end.writeUInt32LE(endNamespace ? index(endNamespace) : NONE); end.writeUInt32LE(index(item.name), 4); xml.push(chunk(0x103, 16, end));
  }
  emit(tree); if (resourceIds) xml.push(namespace(0x101));
  return chunk(3, 8, Buffer.concat(xml));
}

module.exports={binaryXml};
