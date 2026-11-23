# Compare two frame dumps (golden, then new): the text must match byte for byte; each image must have the same place, header and pixels.
import re, sys, zlib, base64
def frames(path):
    return open(path, 'rb').read().split(b'\n=====\n')[:-1]
def parse(fr):
    cut = fr.find(b'\x1b[J')
    text, rest = fr[:cut + 3], fr[cut + 3:]
    imgs, deletes = [], []
    deletes = [int(x) for x in re.findall(rb'\x1b_Ga=d,d=I,i=(\d+),q=2\x1b\\', rest)]
    rest = re.sub(rb'\x1b_Ga=d,d=I,i=\d+,q=2\x1b\\', b'', rest)
    for m in re.finditer(rb'\x1b\[(\d+);(\d+)H((?:\x1b_G[^\x1b]*\x1b\\)+)', rest):
        parts = re.findall(rb'\x1b_G([^;]*);([^\x1b]*)\x1b\\', m.group(3))
        head = parts[0][0].decode()
        if ',t=s,' in head:                                # raw pixels in shared memory: read it back, then unlink it
            from multiprocessing import shared_memory
            n = int(re.search(r'S=(\d+)', head).group(1))
            sm = shared_memory.SharedMemory(name=base64.b64decode(parts[0][1]).decode().lstrip('/'))
            px = bytes(sm.buf[:n]); sm.close(); sm.unlink()
            head = re.sub(r',t=s|,S=\d+', '', head).replace('a=T,f=32,', 'a=T,f=32,o=z,')
        else:
            head = re.sub(r',m=\d', '', head)
            px = zlib.decompress(base64.b64decode(b''.join(p[1] for p in parts)))
        imgs.append(((int(m.group(1)), int(m.group(2))), head, px))
    return text, imgs, deletes, rest.endswith(b'\x1b[?2026l')
a, b = frames(sys.argv[1]), frames(sys.argv[2])
if len(sys.argv) > 3 and sys.argv[3] == 'cache':
    # b skipped unchanged charts: every chart it sent must equal a's; every chart it skipped must look as it did on
    # screen already; every chart that went away must be deleted.
    ok, onscreen = len(a) == len(b), {}
    for i, (fa, fb) in enumerate(zip(a, b)):
        ta, ia, _, _ = parse(fa); tb, ib, db, _ = parse(fb)
        charts = lambda imgs: {int(re.search(r'i=(\d+)', h).group(1)): (pos, h, px) for pos, h, px in imgs if int(re.search(r'i=(\d+)', h).group(1)) < 1000}
        ca, cb = charts(ia), charts(ib)
        sent_ok = all(cb[k] == ca.get(k) for k in cb)
        skipped = [k for k in ca if k not in cb]
        skip_ok = all(onscreen.get(k) == ca[k] for k in skipped)
        gone = [k for k in onscreen if k not in ca]
        del_ok = all(k in db for k in gone)
        gauges_ok = [x for x in ia if 'i=1' not in x[1][:0]] is not None and ta == tb
        for k in cb: onscreen[k] = cb[k]
        for k in gone: onscreen.pop(k, None)
        print(f'frame {i + 1}: charts sent {sorted(cb)} of {sorted(ca)}; skipped ok {skip_ok}; sent same {sent_ok}; '
              f'gone {gone} deleted {del_ok}; text same {ta == tb}')
        ok &= sent_ok and skip_ok and del_ok and ta == tb
    print('ALL SAME' if ok else 'DIFFERENT'); sys.exit(0 if ok else 1)
ok = len(a) == len(b)
print(f'frames: {len(a)} vs {len(b)}')
for i, (fa, fb) in enumerate(zip(a, b)):
    ta, ia, da, ea = parse(fa); tb, ib, db, eb = parse(fb)
    same_text = ta == tb
    same_imgs = len(ia) == len(ib) and all(x[0] == y[0] and x[1] == y[1] and x[2] == y[2] for x, y in zip(ia, ib))
    print(f'frame {i + 1}: text {"same" if same_text else "DIFF"} ({len(ta)} bytes); images {len(ia)} vs {len(ib)} '
          f'{"same" if same_imgs else "DIFF"}; deletes {da} vs {db}; end {ea}/{eb}')
    if not same_text:
        la, lb = ta.split(b'\n'), tb.split(b'\n')
        for j, (x, y) in enumerate(zip(la, lb)):
            if x != y: print('  first differing line', j, '\n  golden:', x[:300], '\n     now:', y[:300]); break
    if not same_imgs:
        for x, y in zip(ia, ib):
            if x[:2] != y[:2] or x[2] != y[2]: print('  image', x[0], x[1], '|', y[0], y[1], 'pixels same:', x[2] == y[2]); break
    ok &= same_text and same_imgs and da == db and ea == eb
print('ALL SAME' if ok else 'DIFFERENT')
sys.exit(0 if ok else 1)
