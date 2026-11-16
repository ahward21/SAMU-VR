"""Minimal UE3 (UDK) package reader: LZO chunk decompression, name/import/export tables, rough bytecode refs.
Read-only inspection tool for the game's script package."""
import struct, sys, os, pickle

TAG = 0x9E2A83C1


def lzo1x_decompress(src, out_len):
    dst = bytearray()
    ip = 0
    n = len(src)
    t = src[ip]
    state = 0  # 0 = main loop top, 1 = first_literal_run, 2 = match, 3 = match_next
    if t > 17:
        ip += 1
        t -= 17
        if t < 4:
            state = 3
        else:
            dst += src[ip:ip + t]
            ip += t
            state = 1
    while True:
        if state == 0:
            t = src[ip]; ip += 1
            if t >= 16:
                state = 2
                continue
            if t == 0:
                while src[ip] == 0:
                    t += 255; ip += 1
                t += 15 + src[ip]; ip += 1
            dst += src[ip:ip + t + 3]
            ip += t + 3
            state = 1
            continue
        if state == 1:
            t = src[ip]; ip += 1
            if t >= 16:
                state = 2
                continue
            m_pos = len(dst) - (1 + 0x0800) - (t >> 2) - (src[ip] << 2); ip += 1
            for _ in range(3):
                dst.append(dst[m_pos]); m_pos += 1
            # match_done
            t = src[ip - 2] & 3
            if t == 0:
                state = 0
            else:
                state = 3
            continue
        if state == 2:
            # match
            if t >= 64:
                m_pos = len(dst) - 1 - ((t >> 2) & 7) - (src[ip] << 3); ip += 1
                t = (t >> 5) - 1
                length = t + 2
            elif t >= 32:
                t &= 31
                if t == 0:
                    while src[ip] == 0:
                        t += 255; ip += 1
                    t += 31 + src[ip]; ip += 1
                m_pos = len(dst) - 1 - ((src[ip] >> 2) + (src[ip + 1] << 6)); ip += 2
                length = t + 2
            elif t >= 16:
                m_pos = len(dst) - ((t & 8) << 11)
                t &= 7
                if t == 0:
                    while src[ip] == 0:
                        t += 255; ip += 1
                    t += 7 + src[ip]; ip += 1
                m_pos -= (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2
                if m_pos == len(dst):
                    break  # EOF
                m_pos -= 0x4000
                length = t + 2
            else:
                m_pos = len(dst) - 1 - (t >> 2) - (src[ip] << 2); ip += 1
                length = 2
            if m_pos < 0:
                raise ValueError("lzo: bad match")
            dist = len(dst) - m_pos
            if dist >= length:
                dst += dst[m_pos:m_pos + length]
            else:
                for _ in range(length):
                    dst.append(dst[m_pos]); m_pos += 1
            t = src[ip - 2] & 3
            state = 0 if t == 0 else 3
            continue
        if state == 3:
            dst += src[ip:ip + t]
            ip += t
            t = src[ip]; ip += 1
            state = 2
            continue
    if len(dst) != out_len:
        raise ValueError("lzo: size mismatch %d != %d" % (len(dst), out_len))
    return bytes(dst)


class Package:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        raw = self.f.read(4096)
        self.chunks = []
        self.cache = {}
        self._parse_summary(raw)

    # --- raw / decompressed reads ---
    def read(self, off, size):
        if not self.chunks:
            self.f.seek(off)
            return self.f.read(size)
        out = bytearray()
        end = off + size
        for i, (uo, us, co, cs) in enumerate(self.chunks):
            if uo + us <= off or uo >= end:
                continue
            data = self._chunk(i)
            a = max(off, uo) - uo
            b = min(end, uo + us) - uo
            out += data[a:b]
        return bytes(out)

    def _chunk(self, i):
        if i in self.cache:
            return self.cache[i]
        uo, us, co, cs = self.chunks[i]
        self.f.seek(co)
        raw = self.f.read(cs)
        tag, bsize, csum, usum = struct.unpack_from('<IIII', raw, 0)
        assert tag == TAG, hex(tag)
        nblocks = (usum + bsize - 1) // bsize
        pos = 16
        blocks = []
        for _ in range(nblocks):
            blocks.append(struct.unpack_from('<II', raw, pos)); pos += 8
        out = bytearray()
        for (bc, bu) in blocks:
            out += lzo1x_decompress(raw[pos:pos + bc], bu)
            pos += bc
        assert len(out) == us, (len(out), us)
        if len(self.cache) > 64:
            self.cache.pop(next(iter(self.cache)))
        self.cache[i] = bytes(out)
        return self.cache[i]

    def _parse_summary(self, raw):
        p = 0
        def i32():
            nonlocal p
            v = struct.unpack_from('<i', raw, p)[0]; p += 4; return v
        tag = struct.unpack_from('<I', raw, 0)[0]; p = 4
        assert tag == TAG, "not a UE3 package"
        self.ver, self.lic = struct.unpack_from('<HH', raw, p); p += 4
        self.header_size = i32()
        n = i32()
        if n < 0:
            p += -n * 2
        else:
            p += n
        self.flags = struct.unpack_from('<I', raw, p)[0]; p += 4
        self.name_count = i32(); self.name_off = i32()
        self.export_count = i32(); self.export_off = i32()
        self.import_count = i32(); self.import_off = i32()
        self.depends_off = i32()
        if self.ver >= 623:
            p += 12
        if self.ver >= 584:
            p += 4
        p += 16  # guid
        gens = i32()
        p += gens * 12
        self.engine_ver = i32(); self.cooker_ver = i32()
        self.comp_flags = i32()
        nchunks = i32()
        for _ in range(nchunks):
            self.chunks.append(struct.unpack_from('<iiii', raw, p)); p += 16

    def load_tables(self):
        # names
        size_guess = self.import_off - self.name_off if self.import_off > self.name_off else self.export_off - self.name_off
        data = self.read(self.name_off, size_guess + 16)
        p = 0
        self.names = []
        for _ in range(self.name_count):
            n = struct.unpack_from('<i', data, p)[0]; p += 4
            if n < 0:
                s = data[p:p + (-n) * 2].decode('utf-16-le', 'replace').rstrip('\0'); p += -n * 2
            else:
                s = data[p:p + n].decode('latin-1').rstrip('\0'); p += n
            p += 8
            self.names.append(s)
        # imports
        data = self.read(self.import_off, self.import_count * 28)
        self.imports = []
        for k in range(self.import_count):
            pk, pkn, cl, cln, outer, on, onn = struct.unpack_from('<iiiiiii', data, k * 28)
            self.imports.append((self.names[cl], outer, self.fname(on, onn)))
        # exports
        data = self.read(self.export_off, self.depends_off - self.export_off)
        p = 0
        self.exports = []
        for k in range(self.export_count):
            cls, sup, outer, on, onn, arch = struct.unpack_from('<iiiiii', data, p); p += 24
            p += 8  # object flags
            ssize, soff = struct.unpack_from('<ii', data, p); p += 8
            p += 4  # export flags
            nnet = struct.unpack_from('<i', data, p)[0]; p += 4 + nnet * 4
            p += 16 + 4
            self.exports.append((cls, sup, outer, self.fname(on, onn), ssize, soff))

    def fname(self, idx, num):
        s = self.names[idx] if 0 <= idx < len(self.names) else '?%d' % idx
        return s if num == 0 else '%s_%d' % (s, num - 1)

    def obj_name(self, idx):
        if idx == 0:
            return 'None'
        if idx > 0:
            return self.exports[idx - 1][3] if idx - 1 < len(self.exports) else '?'
        return self.imports[-idx - 1][2] if -idx - 1 < len(self.imports) else '?'

    def obj_class(self, idx):
        if idx > 0:
            return self.obj_name(self.exports[idx - 1][0]) if self.exports[idx - 1][0] else 'Class'
        if idx < 0:
            return self.imports[-idx - 1][0]
        return ''

    def obj_outer(self, idx):
        if idx > 0:
            return self.exports[idx - 1][2]
        if idx < 0:
            return self.imports[-idx - 1][1]
        return 0

    def path_of(self, idx):
        parts = []
        guard = 0
        while idx != 0 and guard < 12:
            parts.append(self.obj_name(idx))
            idx = self.obj_outer(idx)
            guard += 1
        return '.'.join(reversed(parts))

    def valid_obj(self, v):
        return (0 < v <= len(self.exports)) or (0 > v >= -len(self.imports))


OBJ_TOKENS = {0x00: 'local', 0x01: 'self.', 0x02: 'default.', 0x48: 'localout', 0x1C: 'CALLF', 0x20: 'obj',
              0x2D: 'bool', 0x35: 'member', 0x2E: 'cast', 0x29: 'nativeparm', 0x12: 'classctx'}
NAME_TOKENS = {0x1B: 'CALLV', 0x38: 'CALLG', 0x21: 'name'}
PROP_CLASSES = {'IntProperty', 'FloatProperty', 'BoolProperty', 'ObjectProperty', 'StructProperty', 'ByteProperty',
                'NameProperty', 'StrProperty', 'ArrayProperty', 'ClassProperty', 'DelegateProperty',
                'ComponentProperty', 'InterfaceProperty', 'MapProperty'}
NATIVES = {277: 'Trace', 548: 'FastTrace', 225: 'VSize', 226: 'Normal', 229: 'GetAxes', 275: 'vec<<rot', 276: 'vec>>rot',
           0x39: 'cast'}


def refs(pkg, blob):
    out = []
    i = 0
    n = len(blob)
    while i < n - 4:
        b = blob[i]
        if b in OBJ_TOKENS:
            v = struct.unpack_from('<i', blob, i + 1)[0]
            if pkg.valid_obj(v):
                c = pkg.obj_class(v)
                ok = (c in PROP_CLASSES) if b in (0x00, 0x01, 0x02, 0x48, 0x2D, 0x35, 0x29) else \
                     (c == 'Function') if b == 0x1C else True
                if ok and not (b in (0x20, 0x2E, 0x12) and c in PROP_CLASSES):
                    nm = pkg.obj_name(v)
                    if b == 0x1C or b in (0x20, 0x2E, 0x12):
                        nm = pkg.path_of(v)
                    out.append('%s %s' % (OBJ_TOKENS[b], nm))
                    i += 5
                    continue
        if b in NAME_TOKENS and i + 9 <= n:
            idx, num = struct.unpack_from('<ii', blob, i + 1)
            if 0 <= idx < len(pkg.names) and num == 0 and idx != 0:
                out.append('%s %s' % (NAME_TOKENS[b], pkg.names[idx]))
                i += 9
                continue
        if b in (0x61, 0x62) and i + 1 < n:
            nat = ((b - 0x60) << 8) | blob[i + 1]
            if nat in NATIVES:
                out.append('NATIVE %s' % NATIVES[nat])
                i += 2
                continue
        if b == 0x1E and i + 5 <= n:  # float const
            fv = struct.unpack_from('<f', blob, i + 1)[0]
            if 0.001 < abs(fv) < 1e7 and fv == fv:
                out.append('float %g' % fv)
                i += 5
                continue
        if b == 0x1F:  # string const
            j = blob.find(b'\0', i + 1)
            if 0 < j - i - 1 < 80 and all(32 <= c < 127 for c in blob[i + 1:j]):
                out.append('str "%s"' % blob[i + 1:j].decode('latin-1'))
                i = j + 1
                continue
        i += 1
    return out


def main():
    path = sys.argv[1]
    cmd = sys.argv[2] if len(sys.argv) > 2 else 'info'
    pkg = Package(path)
    print('ver=%d lic=%d flags=0x%X names=%d exports=%d imports=%d comp=%d chunks=%d' % (
        pkg.ver, pkg.lic, pkg.flags, pkg.name_count, pkg.export_count, pkg.import_count, pkg.comp_flags, len(pkg.chunks)))
    if cmd == 'info':
        return
    pkg.load_tables()
    if cmd == 'names':
        pat = sys.argv[3].lower()
        print([n for n in pkg.names if pat in n.lower()][:200])
    elif cmd == 'classes':
        for k, e in enumerate(pkg.exports):
            if e[0] == 0:
                print(k + 1, pkg.path_of(k + 1), 'super=' + pkg.obj_name(e[1]))
    elif cmd == 'members':   # list children of a class: functions + properties
        cname = sys.argv[3]
        for k, e in enumerate(pkg.exports):
            if e[2] and pkg.obj_name(e[2]) == cname and pkg.obj_class(e[2]) == 'Class':
                print('%-22s %s (size %d)' % (pkg.obj_class(k + 1), e[3], e[4]))
    elif cmd == 'func':      # dump rough refs of Class.Function
        cname, fname = sys.argv[3].split('.')
        for k, e in enumerate(pkg.exports):
            if e[3] == fname and pkg.obj_class(k + 1) in ('Function', 'State') and e[2] and \
               (pkg.obj_name(e[2]) == cname or pkg.obj_name(pkg.obj_outer(e[2])) == cname):
                blob = pkg.read(e[5], e[4])
                print('=== %s (%d bytes)' % (pkg.path_of(k + 1), e[4]))
                print('\n'.join(refs(pkg, blob)))
    elif cmd == 'grep':      # which functions reference a name / object
        pat = sys.argv[3]
        only = sys.argv[4] if len(sys.argv) > 4 else None
        for k, e in enumerate(pkg.exports):
            if pkg.obj_class(k + 1) != 'Function':
                continue
            if only and only not in pkg.path_of(k + 1):
                continue
            blob = pkg.read(e[5], e[4])
            r = refs(pkg, blob)
            if any(pat in x for x in r):
                print(pkg.path_of(k + 1))
    elif cmd == 'defaults':  # raw dump of an export's serial data (hex + refs)
        name = sys.argv[3]
        for k, e in enumerate(pkg.exports):
            if e[3] == name:
                blob = pkg.read(e[5], e[4])
                print('=== %s class=%s size=%d' % (pkg.path_of(k + 1), pkg.obj_class(k + 1), e[4]))
                print(blob[:int(sys.argv[4]) if len(sys.argv) > 4 else 256].hex())


if __name__ == '__main__':
    main()
