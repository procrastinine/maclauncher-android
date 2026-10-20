// Android binary XML metadata only. Guest code is never loaded during inspection.
// Format: AOSP androidfw/ResourceTypes.h (ResXMLTree and ResStringPool).
const ANDROID = "http://schemas.android.com/apk/res/android";
const NONE = 0xffffffff;
const INSPECTION_VERSION = 10;
const LIMITS = Object.freeze({ bytes: 2 * 1024 * 1024, strings: 32768, nodes: 8192, attributes: 256, depth: 64 });

function readBinaryManifest(bytes) {
  function invalid() { throw new Error("The APK contains an invalid Android manifest."); }
  if (!Buffer.isBuffer(bytes) || bytes.length < 8 || bytes.length > LIMITS.bytes) invalid();
  function range(at, length, end = bytes.length) {
    if (!Number.isSafeInteger(at) || at < 0 || length < 0 || at > end - length) invalid();
  }
  function chunk(at, end) {
    range(at, 8, end);
    const type = bytes.readUInt16LE(at), header = bytes.readUInt16LE(at + 2), size = bytes.readUInt32LE(at + 4);
    if (header < 8 || size < header) invalid();
    range(at, size, end);
    return { type, header, size, end: at + size };
  }
  const root = chunk(0, bytes.length);
  if (root.type !== 3 || root.end !== bytes.length) invalid();
  let strings = null, nodes = 0, document = null;
  const stack = [];
  function string(index) {
    if (index === NONE) return null;
    if (!strings || index >= strings.length) invalid();
    return strings[index];
  }
  function pool(at, part) {
    if (strings || part.header < 28) invalid();
    const count = bytes.readUInt32LE(at + 8), styles = bytes.readUInt32LE(at + 12);
    const utf8 = (bytes.readUInt32LE(at + 16) & 0x100) !== 0;
    const start = bytes.readUInt32LE(at + 20), styleStart = bytes.readUInt32LE(at + 24);
    if (count > LIMITS.strings || styles > count || start < part.header + (count + styles) * 4 || start > part.size) invalid();
    const end = styleStart ? at + styleStart : part.end;
    if (end < at + start || end > part.end) invalid();
    range(at + part.header, (count + styles) * 4, at + start);
    const decoded = [], utf8Decoder = new TextDecoder("utf-8", { fatal: true });
    for (let i = 0; i < count; i++) {
      let cursor = at + start + bytes.readUInt32LE(at + part.header + i * 4);
      function length8() {
        range(cursor, 1, end); let value = bytes[cursor++];
        if (value & 0x80) { range(cursor, 1, end); value = ((value & 0x7f) << 8) | bytes[cursor++]; }
        return value;
      }
      let text;
      if (utf8) {
        const units = length8(), size = length8(); range(cursor, size + 1, end);
        if (bytes[cursor + size] !== 0) invalid();
        text = utf8Decoder.decode(bytes.subarray(cursor, cursor + size));
        if (text.length !== units) invalid();
      } else {
        range(cursor, 2, end); let units = bytes.readUInt16LE(cursor); cursor += 2;
        if (units & 0x8000) { range(cursor, 2, end); units = (units & 0x7fff) * 65536 + bytes.readUInt16LE(cursor); cursor += 2; }
        range(cursor, units * 2 + 2, end);
        if (bytes.readUInt16LE(cursor + units * 2) !== 0) invalid();
        text = bytes.toString("utf16le", cursor, cursor + units * 2);
      }
      if (text.includes("\0")) invalid();
      decoded.push(text);
    }
    strings = decoded;
  }
  for (let at = root.header; at < root.end;) {
    const part = chunk(at, root.end);
    if (part.type >= 0x100 && part.type <= 0x17f && part.header < 16) invalid();
    if (part.type === 1) pool(at, part);
    else if (part.type === 0x102) {
      if (++nodes > LIMITS.nodes || stack.length >= LIMITS.depth || part.header < 16) invalid();
      const ext = at + part.header; range(ext, 20, part.end);
      const ns = string(bytes.readUInt32LE(ext)) || null, name = string(bytes.readUInt32LE(ext + 4));
      if (!name) invalid();
      const offset = bytes.readUInt16LE(ext + 8), size = bytes.readUInt16LE(ext + 10), count = bytes.readUInt16LE(ext + 12);
      if (offset < 20 || size < 20 || count > LIMITS.attributes) invalid();
      range(ext + offset, count * size, part.end);
      const attrs = new Map(), typedAttrs = !ns && name === "meta-data" ? new Map() : null;
      for (let i = 0; i < count; i++) {
        const a = ext + offset + i * size, namespace = string(bytes.readUInt32LE(a));
        const attrName = string(bytes.readUInt32LE(a + 4)), raw = string(bytes.readUInt32LE(a + 8));
        if (!attrName || bytes.readUInt16LE(a + 12) !== 8 || bytes[a + 14] !== 0) invalid();
        const type = bytes[a + 15], data = bytes.readUInt32LE(a + 16);
        const key = (namespace || "") + ":" + attrName;
        if (attrs.has(key)) invalid();
        const value = type === 3 ? string(data) : type === 0x12 ? data !== 0 :
          type === 0x10 || type === 0x11 ? data : type === 1 ? { resource: data } : raw;
        attrs.set(key, value);
        if (typedAttrs) typedAttrs.set(key, { type, data, value,
          ...(type === 4 ? { float: bytes.readFloatLE(a + 16) } : {}) });
      }
      // Distribution and platform extensions may use their own namespaces.
      // Keep their expanded XML identity for structural validation, but do not
      // mistake an extension's local name for an Android manifest declaration.
      const node = { name: ns ? null : name, localName: name, namespace: ns, attrs, typedAttrs, children: [] };
      if (stack.length) stack.at(-1).children.push(node);
      else { if (document) invalid(); document = node; }
      stack.push(node);
    } else if (part.type === 0x103) {
      if (part.header < 16) invalid();
      const ext = at + part.header; range(ext, 8, part.end);
      const ns = string(bytes.readUInt32LE(ext)) || null, name = string(bytes.readUInt32LE(ext + 4)), opened = stack.pop();
      if (!opened || opened.namespace !== ns || opened.localName !== name) invalid();
    } else if (part.type === 0x104) {
      // Match ResXMLParser::nextNode: this event requires cdataExt storage,
      // not an initialized typed value. Character data does not alter manifest
      // declarations or nesting, so inspection need not interpret its value.
      if (part.header < 16) invalid();
      const ext = at + part.header; range(ext, 12, part.end);
    }
    // ResXMLParser skips unknown event/chunk types after structural bounds
    // validation. Optional extensions must not prevent metadata inspection.
    at = part.end;
  }
  if (stack.length || document?.name !== "manifest") invalid();
  return document;
}

function inspectManifest(document) {
  function invalid() { throw new Error("The APK contains an invalid Android manifest."); }
  const attribute = (node, name, android = true) => node?.attrs.get((android ? ANDROID : "") + ":" + name);
  // Only required declarations constrain provider selection. The original
  // PackageParser still supplies the complete feature inventory to Android.
  let requiredGlEsVersion = 0;
  for (const feature of document.children.filter(node => node.name === "uses-feature")) {
    const version = attribute(feature, "glEsVersion");
    if (attribute(feature, "required") !== false && Number.isInteger(version) && version > 0 && version <= 0xffffffff)
      requiredGlEsVersion = Math.max(requiredGlEsVersion, version);
  }
  const packageName = attribute(document, "package", false);
  if (typeof packageName !== "string" || !/^[A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)+$/.test(packageName)) invalid();
  function className(value) {
    if (typeof value !== "string" || !/^[.\w$]+$/.test(value)) return null;
    return value.startsWith(".") ? packageName + value : value.includes(".") ? value : packageName + "." + value;
  }
  const applications = document.children.filter(node => node.name === "application");
  if (applications.length !== 1) invalid();
  const app = applications[0], launchers = [];
  const unknownProcess = Symbol("uninspected process");
  const processOf = (owner, fallback) => {
    const value = attribute(owner, "process") || fallback;
    if (typeof value !== "string") throw unknownProcess;
    const normalized = value.startsWith(":") ? packageName + value : value;
    if (!/^[A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*(?::[A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*)?$/.test(normalized)) throw unknownProcess;
    return normalized;
  };
  function processInventory() {
    try {
      const applicationProcessName = processOf(app, packageName);
      return { applicationProcessesInspected: true, applicationProcessName,
        applicationProcesses: [...new Set([applicationProcessName, ...app.children
          .filter(node => ["activity", "service", "provider", "receiver"].includes(node.name))
          .map(node => processOf(node, applicationProcessName))])] };
    } catch (error) { if (error !== unknownProcess) throw error; return { applicationProcessesInspected: false }; }
  }
  const activities = app.children.filter(node => node.name === "activity" || node.name === "activity-alias");
  for (const activity of activities) {
    if (attribute(app, "enabled") === false || attribute(activity, "enabled") === false) continue;
    const declared = className(attribute(activity, "name"));
    const target = activity.name === "activity-alias" ? className(attribute(activity, "targetActivity")) : declared;
    if (!declared || !target) continue;
    if (activity.name === "activity-alias" && !activities.some(node => node.name === "activity" &&
      className(attribute(node, "name")) === target) && !attribute(document, "split", false)) invalid();
    for (const filter of activity.children.filter(node => node.name === "intent-filter")) {
      const main = filter.children.some(node => node.name === "action" && attribute(node, "name") === "android.intent.action.MAIN");
      const launcher = filter.children.some(node => node.name === "category" && attribute(node, "name") === "android.intent.category.LAUNCHER");
      if (main && launcher && !launchers.some(item => item.name === declared)) launchers.push({ name: declared, activity: target });
    }
  }
  const sdk = document.children.find(node => node.name === "uses-sdk");
  const versionName = attribute(document, "versionName"), label = attribute(app, "label");
  // Preserve the distinction between an inspected absence and data requiring
  // unsupported Android resource types. Never manufacture an empty Bundle.
  function applicationMetadata(owner = app) {
    const entries = Object.create(null);
    for (const node of owner.children.filter(node => node.name === "meta-data")) {
      const name = attribute(node, "name"), resource = node.typedAttrs.get(ANDROID + ":resource");
      const raw = node.typedAttrs.get(ANDROID + ":value");
      let entry;
      if (typeof name !== "string" || !name) return { applicationMetadataInspected: false };
      if (resource?.type === 1 && resource.data !== 0) entry = { type: "resource", value: resource.data };
      else if (resource && !(resource.type === 1 && resource.data === 0)) return { applicationMetadataInspected: false };
      else if (raw?.type === 3 && typeof raw.value === "string") entry = { type: "string", value: raw.value };
      else if (raw?.type === 0x12) entry = { type: "boolean", value: raw.data !== 0 };
      else if (raw?.type === 0x10 || raw?.type === 0x11 || raw?.type >= 0x1c && raw?.type <= 0x1f)
        entry = { type: "int", value: raw.data | 0 };
      else if (raw?.type === 4 && Number.isFinite(raw.float)) entry = { type: "float", value: Object.is(raw.float, -0) ? "-0" : raw.float };
      else if (raw?.type === 1 && raw.data !== 0) entry = { type: "reference", value: raw.data };
      else return { applicationMetadataInspected: false };
      // Android's Bundle parser lets the last declaration replace an earlier
      // value for the same name. Resource values resolve in the Android host.
      entries[name] = entry;
    }
    return { applicationMetadataInspected: true,
      applicationMetadata: Object.keys(entries).length ? entries : null };
  }
  function applicationComponents() {
    // Keep unknown resource-backed attributes distinct from inspected defaults.
    // Older hosts can still use the rest of the package inspection.
    const unknown = Symbol("uninspected component");
    const boolean = (owner, key, fallback, allowReference = false) => {
      const value = attribute(owner, key);
      if (value === undefined) return fallback;
      if (allowReference && Number.isInteger(value?.resource) && value.resource > 0) return value;
      if (typeof value !== "boolean") throw unknown;
      return value;
    };
    const text = (owner, key, fallback = null, allowReference = false) => {
      const value = attribute(owner, key);
      if (value === undefined) return fallback;
      if (allowReference && Number.isInteger(value?.resource) && value.resource > 0) return value;
      if (typeof value !== "string") throw unknown;
      return value;
    };
    const processName = processOf;
    try {
      const applicationProcessName = processName(app, packageName);
      const appEnabled = boolean(app, "enabled", true), appPermission = text(app, "permission");
      const providerNodes = app.children.filter(node => node.name === "provider");
      const serviceNodes = app.children.filter(node => node.name === "service");
      const receiverNodes = app.children.filter(node => node.name === "receiver");
      if (providerNodes.length > 128 || serviceNodes.length > 128 || receiverNodes.length > 128) throw unknown;
      const names = new Set();
      function common(owner) {
        const name = className(attribute(owner, "name")), metadata = applicationMetadata(owner);
        if (!name || !/^[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)+$/.test(name) ||
            names.has(owner.name + ":" + name) || !metadata.applicationMetadataInspected) throw unknown;
        names.add(owner.name + ":" + name);
        return { name, processName: processName(owner, applicationProcessName),
          enabled: appEnabled && boolean(owner, "enabled", true, true),
          directBootAware: boolean(owner, "directBootAware", false, true), metadata: metadata.applicationMetadata };
      }
      const applicationProviders = providerNodes.map(owner => {
        const info = common(owner), authority = text(owner, "authorities", null, true);
        if (!authority) throw unknown;
        if (typeof authority === "string") {
          const aliases = authority.split(";");
          if (aliases.length > 64 || aliases.some(value => !value.trim() || value.length > 1024)) throw unknown;
        }
        const permission = text(owner, "permission", appPermission), order = attribute(owner, "initOrder");
        const initOrder = order === undefined ? 0 : order;
        const target = attribute(sdk, "targetSdkVersion"), minimum = attribute(sdk, "minSdkVersion");
        const targetSdk = target === undefined ? minimum === undefined ? 1 : minimum : target;
        if (!Number.isInteger(initOrder) || initOrder < 0 || initOrder > 0xffffffff ||
            !Number.isInteger(targetSdk) || targetSdk < 1 || targetSdk > 0x7fffffff ||
            owner.children.some(node => ["path-permission", "grant-uri-permission"].includes(node.name))) throw unknown;
        return { ...info, authority, exported: boolean(owner, "exported", targetSdk < 17, true),
          grantUriPermissions: boolean(owner, "grantUriPermissions", false, true), multiprocess: boolean(owner, "multiprocess", false, true),
          initOrder: initOrder | 0, readPermission: text(owner, "readPermission", permission) || null,
          writePermission: text(owner, "writePermission", permission) || null };
      });
      const applicationServices = serviceNodes.map(owner => {
        // This profile cannot reproduce Android's additional service process
        // or task lifecycles. Do not silently turn those into ordinary services.
        for (const flag of ["isolatedProcess", "stopWithTask", "externalService", "useAppZygote", "singleUser", "allowSharedIsolatedProcess"])
          if (boolean(owner, flag, false)) throw unknown;
        return { ...common(owner),
          exported: boolean(owner, "exported", owner.children.some(node => node.name === "intent-filter"), true),
          permission: text(owner, "permission", appPermission) || null };
      });
      const applicationReceivers = receiverNodes.map(owner => ({ ...common(owner),
        exported: boolean(owner, "exported", owner.children.some(node => node.name === "intent-filter"), true),
        permission: text(owner, "permission", appPermission) || null }));
      const launcherProcesses = Object.create(null);
      for (const launcher of launchers) {
        const target = activities.find(node => node.name === "activity" && className(attribute(node, "name")) === launcher.activity);
        if (!target) throw unknown;
        launcherProcesses[launcher.name] = processName(target, applicationProcessName);
      }
      return { applicationComponentsInspected: true, applicationProcessName, launcherProcesses,
        applicationProviders, applicationServices, applicationReceivers };
    } catch (error) {
      if (error !== unknown && error !== unknownProcess) throw error;
      return { applicationComponentsInspected: false };
    }
  }
  function applicationActivities() {
    const unknown = Symbol("uninspected activity");
    function flag(owner,key,fallback) {
      const value=attribute(owner,key);
      if(value===undefined)return fallback;
      if(typeof value==="boolean"||Number.isInteger(value?.resource)&&value.resource>0)return value;
      throw unknown;
    }
    function process(owner) {
      return processOf(owner,processOf(app,packageName));
    }
    function permission(owner,fallback) {
      const value=attribute(owner,"permission")??fallback;
      if(value==null||value==="")return null;
      if(typeof value!=="string")throw unknown;
      return value;
    }
    try {
      if(activities.length>256)throw unknown;
      const seen=new Set(),appEnabled=flag(app,"enabled",true);
      const applicationActivities=activities.map(owner=>{
        const name=className(attribute(owner,"name"));
        if(!name||seen.has(name))throw unknown;seen.add(name);
        const targetActivity=owner.name==="activity-alias"?className(attribute(owner,"targetActivity")):null;
        const target=targetActivity?activities.find(node=>node.name==="activity"&&className(attribute(node,"name"))===targetActivity):owner;
        if(!target)throw unknown;
        const metadata=applicationMetadata(owner);if(!metadata.applicationMetadataInspected)throw unknown;
        return {name,targetActivity,processName:process(target),
          applicationEnabled:appEnabled,enabled:flag(owner,"enabled",true),
          exported:flag(owner,"exported",owner.children.some(node=>node.name==="intent-filter")),
          directBootAware:flag(target,"directBootAware",false),
          permission:permission(owner,permission(target,permission(app,null))),metadata:metadata.applicationMetadata};
      });
      return {applicationActivitiesInspected:true,applicationActivities};
    } catch(error) {
      if(error!==unknown&&error!==unknownProcess)throw error;
      return {applicationActivitiesInspected:false};
    }
  }
  function themeResource(owner) {
    const value=attribute(owner,"theme");
    if(value==null)return {inspected:true,resource:0};
    if(Number.isInteger(value?.resource)&&value.resource>=0&&value.resource<=0xffffffff)
      return {inspected:true,resource:value.resource};
    return {inspected:false};
  }
  const applicationTheme=themeResource(app),activityThemes=Object.create(null);
  for(const owner of activities)if(owner.name==="activity")activityThemes[className(attribute(owner,"name"))]=themeResource(owner);
  const icon = attribute(app, "icon");
  const applicationIconInspected = icon === undefined || Number.isInteger(icon?.resource);
  const activityMetadata = Object.create(null);
  for (const launcher of launchers) {
    if (Object.hasOwn(activityMetadata, launcher.activity)) continue;
    const target = activities.find(node => node.name === "activity" && className(attribute(node, "name")) === launcher.activity);
    const inspected = target && applicationMetadata(target);
    activityMetadata[launcher.activity] = inspected?.applicationMetadataInspected === true
      ? { inspected: true, entries: inspected.applicationMetadata } : { inspected: false };
  }
  return { packageName, versionName: typeof versionName === "string" ? versionName : null,
    manifestInspectionVersion: INSPECTION_VERSION, requiredGlEsVersion,
    versionCode: attribute(document, "versionCode") ?? null,
    versionCodeMajor: attribute(document, "versionCodeMajor") ?? 0,
    label: typeof label === "string" ? label : null,
    debuggable: attribute(app, "debuggable") === true,
    hasCode: attribute(app, "hasCode") !== false,
    applicationTheme, activityThemes,
    applicationIconInspected,
    ...(applicationIconInspected ? { applicationIcon: icon?.resource ?? 0 } : {}),
    ...applicationMetadata(), ...applicationComponents(), ...applicationActivities(),
    ...processInventory(),
    applicationClass: className(attribute(app, "name")), launchers, activityMetadata,
    minSdk: attribute(sdk, "minSdkVersion") ?? 1, targetSdk: attribute(sdk, "targetSdkVersion") ?? null,
    split: attribute(document, "split", false) || null,
    isolatedSplits: attribute(document, "isolatedSplits") === true,
    isFeatureSplit: attribute(document, "isFeatureSplit") === true,
    configForSplit: attribute(document, "configForSplit", false) || null,
    requiredSplits: document.children.filter(node => node.name === "uses-split").map(node => attribute(node, "name")).filter(value => typeof value === "string"),
    requiresSplitInstallation: attribute(document, "isSplitRequired") === true || attribute(app, "isSplitRequired") === true || app.children.some(node => node.name === "meta-data" &&
      attribute(node, "name") === "com.android.vending.splits.required" && attribute(node, "value") === true),
    permissions: [...new Set(document.children.filter(node => node.name === "uses-permission" || node.name === "uses-permission-sdk-23")
      .map(node => attribute(node, "name")).filter(value => typeof value === "string"))] };
}

function parseBinaryManifest(bytes) { return inspectManifest(readBinaryManifest(bytes)); }

/** Inspection follows parseSplitApplication's base defaults. The original
 * PackageParser still owns actual component registration and split loaders. */
function parseBinaryManifestSet(manifests) {
  const documents = manifests.map(readBinaryManifest), base = documents[0];
  if (!base || base.attrs.get(":split")) throw new Error("An Android manifest set requires its base first.");
  const application = document => {
    const applications = document.children.filter(node => node.name === "application");
    if (applications.length !== 1) throw new Error("The APK contains an invalid Android manifest.");
    return applications[0];
  };
  const app = application(base);
  // Android parses cluster splits by their manifest names, independently of
  // the topological order used to validate the installation dependencies.
  const splits = documents.slice(1).sort((a, b) => {
    const left = String(a.attrs.get(":split")), right = String(b.attrs.get(":split"));
    return left < right ? -1 : left > right ? 1 : 0;
  });
  const children = [...app.children, ...splits.flatMap(document => application(document).children)];
  return inspectManifest({ ...base, children: base.children.map(node => node === app ? { ...app, children } : node) });
}

module.exports = { LIMITS, INSPECTION_VERSION, parseBinaryManifest, parseBinaryManifestSet, readBinaryManifest };
