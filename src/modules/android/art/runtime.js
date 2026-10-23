const fs = require("node:fs"), path = require("node:path");
const definition = require("./supplier.json");
const componentPaths = require("./components.json");
const attachedSuppliers = new WeakMap();

// An ART supplier is part of a verified Android payload. The normal verifier
// already checks every file; this module resolves its relocation-safe schema.
function member(root, value) {
  if (typeof value !== "string" || !value || path.isAbsolute(value) ||
      value.split(/[\\/]/).some(part => !part || part === "." || part === "..") || /[\0\r\n\t]/.test(value))
    throw new Error("The Android ART component path is invalid.");
  const file = path.join(root, value);
  // Supplier files may contain original in-tree symlinks, but never point to
  // another installed version or a developer's build directory.
  const actual = fs.realpathSync(file), relative = path.relative(fs.realpathSync(root), actual);
  if (relative === ".." || relative.startsWith(".." + path.sep) || path.isAbsolute(relative))
    throw new Error("The Android ART component is outside its installed runtime.");
  return file;
}

function validateManifest(manifest) {
  if (manifest.binderSenderIdentityVersion !== undefined && manifest.binderSenderIdentityVersion !== 1)
    throw new Error("The Android Binder identity version is unsupported.");
  if (manifest.schemaVersion !== 1 || manifest.execution !== "art" ||
      manifest.supplier !== definition.id || manifest.hostcallVersion !== definition.hostcallVersion ||
      manifest.android?.imageSha256 !== definition.android.imageSha256 ||
      manifest.android?.buildId !== definition.android.buildId ||
      !Array.isArray(manifest.bootClasspath) || !manifest.bootClasspath.length ||
      manifest.bootClasspath.some(file => typeof file !== "string" || !file.startsWith("/") || /[\0\r\n\t:]/.test(file)))
    throw new Error("The Android ART supplier profile is invalid.");
}

function validateBootImage(components, manifest) {
  if(components.bootImage&&(!manifest.bootImage||manifest.bootImage.location!=="/system/framework/boot.art"||
    !Array.isArray(manifest.bootImage.files)||!manifest.bootImage.files.length||manifest.bootImage.files.some(file=>
      typeof file!=="string"||!file.startsWith("framework/")||file.split(/[\\/]/).some(part=>!part||part==="."||part==="..")||/[\0\r\n\t]/.test(file))))
    throw new Error("The Android ART boot image paths are invalid.");
}

// Only the managed delivery owner attaches a descriptor, after verifying both
// the owned package and its separately acquired original Android image. Keep
// this association out of persisted game settings and imported metadata.
function attachSupplier(payload, descriptor) {
  validateManifest(descriptor?.manifest || {});
  if (!path.isAbsolute(descriptor.root || "") || !descriptor.components ||
      descriptor.packageRoot && (!path.isAbsolute(descriptor.packageRoot) || path.relative(descriptor.packageRoot,descriptor.root).split(path.sep).includes('..')) ||
      Object.keys(componentPaths).some(name => !path.isAbsolute(descriptor.components[name] || "")) ||
      JSON.stringify(descriptor.bootClasspath) !== JSON.stringify(descriptor.manifest.bootClasspath))
    throw new Error("The Android ART supplier descriptor is incomplete.");
  validateBootImage(descriptor.components, descriptor.manifest);
  const bound = { ...payload, profile: { ...payload.profile, artExecutionVersion: 1 } };
  attachedSuppliers.set(bound, Object.freeze({ ...descriptor,
    components: Object.freeze({ ...descriptor.components }), bootClasspath: Object.freeze(descriptor.bootClasspath.slice()),
    entry: definition.applicationEntry, guestEntry: definition.guestEntry }));
  return bound;
}

function resolve(payload) {
  const attached = attachedSuppliers.get(payload);
  if (attached) return attached;
  if (payload.profile?.artExecutionVersion !== 1)
    throw new Error("ART is not included in this Android runtime.");
  const root = member(payload.root, "art");
  const manifest = JSON.parse(fs.readFileSync(path.join(root, "runtime.json"), "utf8"));
  validateManifest(manifest);
  const components = {};
  for (const [name, file] of Object.entries(manifest.components || {})) components[name] = member(root, file);
  for (const name of Object.keys(componentPaths))
    if (!components[name]) throw new Error("The Android ART supplier is incomplete: " + name);
  validateBootImage(components, manifest);
  return { root, manifest, components, bootClasspath: manifest.bootClasspath.slice(),
    entry: definition.applicationEntry, guestEntry: definition.guestEntry };
}
module.exports = { resolve, member, attachSupplier, validateManifest };
