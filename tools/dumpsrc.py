import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upk
pkg = upk.Package(sys.argv[1]); pkg.load_tables()
outdir = sys.argv[2]; os.makedirs(outdir, exist_ok=True)
n = 0
for k, e in enumerate(pkg.exports):
    if e[3] == 'ScriptText' and pkg.obj_class(k + 1) == 'TextBuffer':
        blob = pkg.read(e[5], e[4])
        cname = pkg.obj_name(e[2])
        ln = struct.unpack_from('<i', blob, 20)[0]
        if ln < 0:
            txt = blob[24:24 + (-ln) * 2].decode('utf-16-le', 'replace')
        else:
            txt = blob[24:24 + ln].decode('latin-1')
        open(os.path.join(outdir, cname + '.uc'), 'w', encoding='utf-8', newline='\n').write(txt.rstrip('\0').replace('\r\n', '\n'))
        n += 1
print('wrote', n, 'sources')
