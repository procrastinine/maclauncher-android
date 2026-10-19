import {includeDependencies} from './upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';

function replace(source, before, after) {
  if (source.split(before).length !== 2) throw Error('Klepton I/O boundary changed: ' + before.slice(0, 80));
  return source.replace(before, after);
}

/** Retain original socket ownership, flags and tracing; adapt the address ABI
 * once, before exposing caller-sized buffers to any native socket operation. */
export function prepareIoSources({shim, slink, libc}) {
  let source = fs.readFileSync(shim, 'utf8');
  source = replace(source, 'static void kl_android_log_write(int p, const char *t, const char *m) {\n    fprintf(stderr, "[%d/%s] %s\\n", p, t ? t : "", m ? m : ""); }',
    'static int kl_android_log_write(int p, const char *t, const char *m) {\n    return fprintf(stderr, "[%d/%s] %s\\n", p, t ? t : "", m ? m : ""); }');
  source = replace(source, '    if (!sa || len > sizeof *dst) return -1;\n    memcpy(dst, sa, len);',
    '    if (!sa) { errno = EFAULT; return -1; }\n    if (len < 2 || len > sizeof *dst) { errno = EINVAL; return -1; }\n    memset(dst, 0, sizeof *dst);\n    memcpy(dst, sa, len);');
  source = replace(source, '    unsigned fam = *(const uint16_t *)sa;\n    if (fam == 10) fam = AF_INET6;',
    '    uint16_t fam; memcpy(&fam, sa, sizeof(fam));\n    if (fam == 10) fam = AF_INET6;\n    if (fam > UINT8_MAX) { errno = EAFNOSUPPORT; return -1; }\n    if ((fam == AF_INET && len < sizeof(struct sockaddr_in)) ||\n        (fam == AF_INET6 && len < sizeof(struct sockaddr_in6))) { errno = EINVAL; return -1; }');
  source = replace(source, '    memmove((uint8_t *)sa + 2, (const uint8_t *)sa + 2, (size_t)*len - 2);\n    *(uint16_t *)sa = (uint16_t)fam;',
    '    uint16_t family = (uint16_t)fam; memcpy(sa, &family, sizeof(family));');
  const helper = `
static void klj_copy_socket_address(struct sockaddr *destination, socklen_t *length,
                                   struct sockaddr_storage *address, socklen_t actual,
                                   socklen_t capacity) {
    kl_sa_to_guest((struct sockaddr *)address, &actual);
    socklen_t copied = capacity < actual ? capacity : actual;
    if (copied > sizeof(*address)) copied = sizeof(*address);
    if (copied) memcpy(destination, address, copied);
    *length = actual;
}
static int klj_socket_address_call(int fd, struct sockaddr *address, socklen_t *length,
                                  int (*operation)(int, struct sockaddr *, socklen_t *)) {
    if (!address) return operation(fd, NULL, length);
    if (!length) { errno = EFAULT; return -1; }
    struct sockaddr_storage host = {0};
    socklen_t actual = sizeof(host), capacity = *length;
    int result = operation(fd, (struct sockaddr *)&host, &actual);
    if (result >= 0) klj_copy_socket_address(address, length, &host, actual, capacity);
    return result;
}
`;
  source = replace(source, '// A HOST sockaddr, printed. Shared by every traced call so one address never',
    helper + '\n// A HOST sockaddr, printed. Shared by every traced call so one address never');
  const addressCheck = '    if (kl_sa_to_host(&hs, sa, len) == 0) {';
  if (source.split(addressCheck).length !== 3) throw Error('Klepton connect/bind address conversion changed');
  source = source.replaceAll(addressCheck, '    if (kl_sa_to_host(&hs, sa, len) != 0) return -1;\n    {');
  source = replace(source, '    if (sa && kl_sa_to_host(&hs, sa, len) == 0) {',
    '    if (sa) {\n        if (kl_sa_to_host(&hs, sa, len) != 0) return -1;');
  source = replace(source, '    if (g->msg_name && g->msg_namelen &&\n        kl_sa_to_host(&hs, g->msg_name, g->msg_namelen) == 0) {',
    '    if (g->msg_name && g->msg_namelen) {\n        if (kl_sa_to_host(&hs, g->msg_name, g->msg_namelen) != 0) return -1;');
  for (const operation of ['accept', 'getpeername', 'getsockname']) {
    const begin = source.indexOf(`static int kl_${operation}(int fd, struct sockaddr *sa, socklen_t *len) {`);
    const end = source.indexOf('\n}', begin);
    if (begin < 0 || end < 0) throw Error('Klepton socket address operation changed: ' + operation);
    source = source.slice(0, begin) + `static int kl_${operation}(int fd, struct sockaddr *sa, socklen_t *len) {
    return klj_socket_address_call(fd, sa, len, ${operation});
}` + source.slice(end + 2);
  }
  source = replace(source, '    ssize_t r = recvfrom(fd, buf, n, dflags, sa, len);',
    `    if (sa && !len) { errno = EFAULT; return -1; }
    struct sockaddr_storage address = {0};
    socklen_t actual = sizeof(address), capacity = len ? *len : 0;
    ssize_t r = recvfrom(fd, buf, n, dflags, sa ? (struct sockaddr *)&address : NULL, sa ? &actual : NULL);`);
  source = replace(source, '        kl_sa_fmt(host, sizeof host, sa);\n        kl_sa_to_guest(sa, len);',
    '        kl_sa_fmt(host, sizeof host, (struct sockaddr *)&address);\n        klj_copy_socket_address(sa, len, &address, actual, capacity);');
  source = replace(source, '            if (nl > g->msg_namelen) nl = g->msg_namelen;\n            memcpy(g->msg_name, &hs, nl);\n            kl_sa_to_guest(g->msg_name, &nl);\n            g->msg_namelen = nl;',
    '            klj_copy_socket_address(g->msg_name, &g->msg_namelen, &hs, nl, g->msg_namelen);');
  source = replace(source, '    if (g->msg_name && g->msg_namelen) { h.msg_name = &hs; h.msg_namelen = sizeof hs; }',
    '    if (g->msg_name) { h.msg_name = &hs; h.msg_namelen = sizeof hs; }');
  source = replace(source,'X(klb_stat) X(klb_lstat) X(klb_fstat) X(klb_statfs)',
    'X(klb_stat) X(klb_lstat) X(klb_fstat) X(klb_statfs) X(klb_fstatfs) X(klb_posix_fadvise)');
  source = replace(source,'E("statfs", klb_statfs),',
    'E("statfs", klb_statfs), E("statfs64", klb_statfs), E("fstatfs", klb_fstatfs), E("fstatfs64", klb_fstatfs),\n    E("posix_fadvise", klb_posix_fadvise), E("posix_fadvise64", klb_posix_fadvise),');
  source = replace(source,'E("statvfs", klb_statvfs), E("fstatvfs", klb_fstatvfs),',
    'E("statvfs", klb_statvfs), E("statvfs64", klb_statvfs), E("fstatvfs", klb_fstatvfs), E("fstatvfs64", klb_fstatvfs),');
  fs.writeFileSync(shim, source);
  source = fs.readFileSync(slink, 'utf8');
  const begin = source.indexOf('int klb_ppoll('), end = source.indexOf('\n// ----', begin);
  if (begin < 0 || end < 0) throw Error('Klepton ppoll boundary changed');
  const body = source.slice(begin, end).replaceAll('poll(fds, (nfds_t)nfds, ms)', 'klj_poll(fds, (nfds_t)nfds, ms)');
  fs.writeFileSync(slink, includeDependencies(source.slice(0, begin) + body + source.slice(end), '#include ' + JSON.stringify(path.join(import.meta.dirname, 'poll-bridge.h'))));
  source = fs.readFileSync(libc, 'utf8');
  source = replace(source, 'int klb_statfs(const char *path, void *out) {\n    struct statfs d;\n    if (statfs(path, &d) != 0) return -1;',
    'int klb_statfs(const char *path, void *out) {\n    char mapped_path[PATH_MAX];\n    const char *mapped = kl_guest_path(path, mapped_path, sizeof(mapped_path));\n    struct statfs d;\n    if (statfs(mapped, &d) != 0) return -1;');
  const statStart=source.indexOf('int klb_statfs('),statEnd=source.indexOf('// ---------- getrandom ----------',statStart);
  const statBody=source.slice(statStart,statEnd),fillStart=statBody.indexOf('    if (!out)'),fillEnd=statBody.lastIndexOf('}');
  if(statStart<0||statEnd<0||fillStart<0||fillEnd<0)throw Error('Klepton filesystem-stat owner changed');
  const fill=statBody.slice(fillStart,fillEnd).replace('if (!out) return 0;','if (!out) { errno = EFAULT; return -1; }').replace(/\bd\./g,'d->');
  source=source.slice(0,statStart)+`static int klj_fill_statfs(const struct statfs *d, void *out) {
${fill}}
${statBody.slice(0,fillStart)}    return klj_fill_statfs(&d,out);
}
int klb_fstatfs(int fd, void *out) {
    struct statfs d;
    if (fstatfs(fd,&d)) return -1;
    return klj_fill_statfs(&d,out);
}
// These are Linux advisory requests over the existing file description. The
// hints do not change its file offset or replace its descriptor lifetime.
int klb_posix_fadvise(int fd, int64_t offset, int64_t length, int advice) {
    int saved=errno,error=0;struct stat file;
    if (fstat(fd,&file)) error=errno;
    else if (S_ISFIFO(file.st_mode)||S_ISSOCK(file.st_mode)) error=ESPIPE;
    else if (offset<0||length<0||length>INT64_MAX-offset||advice<0||advice>5) error=EINVAL;
    else if (S_ISREG(file.st_mode)) {
        int result=0;
        if (advice<=2) result=fcntl(fd,F_RDAHEAD,advice!=1);
        else if (advice==3) {
            int64_t count=length?length:file.st_size>offset?file.st_size-offset:0;
            if (count) {struct radvisory range={.ra_offset=offset,.ra_count=(int)(count>INT_MAX?INT_MAX:count)};result=fcntl(fd,F_RDADVISE,&range);}
        }
        // Darwin has no range-only discard/age hint. In particular F_NOCACHE
        // would persistently change later I/O, so DONTNEED/NOREUSE remain
        // optional cache hints, as in original AIM's fadvise64 owner.
        if (result && errno!=ENOTSUP && errno!=ENOTTY) error=errno;
    }
    errno=saved;return kl_errno_to_linux(error);
}

`+source.slice(statEnd);
  for (const name of ['lseek', 'lseek64']) {
    const declaration = new RegExp('(off_t\\s+klb_' + name + '\\(int fd, off_t o, int w\\)\\s*\\{)');
    if ((source.match(new RegExp(declaration.source, 'g')) || []).length !== 1)
      throw Error('Klepton file seek boundary changed: ' + name);
    source = source.replace(declaration, '$1 w = klj_seek_whence_to_host(w);');
  }
  fs.writeFileSync(libc, includeDependencies(source, '#include ' + JSON.stringify(path.join(import.meta.dirname, 'seek-bridge.h'))));
}
