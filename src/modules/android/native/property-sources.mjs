import fs from 'node:fs';

/** Give the existing property provider one enumeration for both find and walk.
 * Values continue to come from its original reader; handles retain its stable
 * name-pointer representation. Hosted Android overrides share this enumeration. */
export function preparePropertySource(file) {
  let source=fs.readFileSync(file,'utf8');
  const start=source.indexOf('const void *klb_sysprop_find(const char *n) {');
  const end=source.indexOf('// PROP_VALUE_MAX',start);
  if(start<0||end<0)throw Error('Klepton property owner changed');
  source=source.slice(0,start)+`
static const char *klj_property_name_at(size_t index) {
    static const char *const computed[] = {
        "ro.build.version.sdk", "net.dns1", "net.dns2", "net.dns3", "net.dns4",
        "debug.hl2q3vr.internal_multiview_probe", "debug.hl2q3vr.array_swapchain"
    };
    size_t count = sizeof computed / sizeof *computed;
    if (index < count) return computed[index];
    index -= count;
    return index < sizeof g_sysprops / sizeof *g_sysprops ? g_sysprops[index].prop : NULL;
}
int klb_sysprop_foreach(void (*callback)(const void *, void *), void *cookie) {
    if (!callback) { errno = EINVAL; return -1; }
    const char *name;
    for (size_t i = 0; (name = klj_property_name_at(i)); ++i)
        if (sysprop_value(name)) callback(name, cookie);
    return 0;
}
const void *klb_sysprop_find(const char *name) {
    if (!name) return NULL;
    const char *property;
    for (size_t i = 0; (property = klj_property_name_at(i)); ++i)
        if (!strcmp(property, name)) return sysprop_value(property) ? property : NULL;
    return NULL;
}

`+source.slice(end);
  fs.writeFileSync(file,source);
}
