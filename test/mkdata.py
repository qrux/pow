# Made-up but plausible live state, written as plain text so pow.py and the C pow draw from the very same numbers.
import random, sys
r = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 7)
L = []
def put(kind, *a):  # kind <tab> fields, each field space-free or a name; values joined by spaces in the last field
    L.append('\t'.join([kind] + [repr(x) if isinstance(x, float) else str(x) for x in a]))
N = 300
put('when', 'Fri Nov 13 21:06:16 2026 +0000')
put('mode', 'High Power · battery 74%')
put('chip', 'Apple M2 Max')
for name, hi in (('ANE', 5000.0), ('GPU', 60000.0), ('CPU', 20000.0)):
    put('hist', name, *[float(round(r.uniform(0, hi))) for _ in range(N)])
put('load_cpu', r.uniform(0, 100)); put('load_gpu', r.uniform(0, 100))
for name in ('CPU', 'GPU'):                         # load % per sample, a few unknown
    put('lhist', name, *['nan' if r.random() < 0.03 else r.uniform(0, 100) for _ in range(N)])
for name, hi in (('E', 34.0), ('P', 66.0)):          # E and P shares of CPU load (4 of 12 cores are E)
    put('lhist', name, *[r.uniform(0, hi) for _ in range(N)])
for i, cl in enumerate(['E'] * 4 + ['P0'] * 4 + ['P1'] * 4):
    put('core', i, cl, r.uniform(0, 100))
soc = ['CPU Energy', 'EACC_CPM', 'EACC_CPM_SRAM', 'PACC0_CPM', 'PACC0_CPM_SRAM', 'PACC1_CPM', 'GPU0', 'GPU CS0', 'GPU SRAM0',
       'GPU CS SRAM0', 'DRAM0', 'DCS0', 'AMCC0', 'ANE0', 'ISP0', 'MSR0', 'PCIe Port 0 Energy', 'apciec1 Energy', 'AVE0',
       'GPU Energy', 'XYZ0', 'QQQ7'] + [f'{p}_CPU{j}' for p in ('EACC', 'PACC0', 'PACC1') for j in range(4)]
for n in soc:
    k = r.randint(5, N)
    put('media', n, *[r.uniform(0, 3000) for _ in range(k)])
put('enc', *[r.uniform(0, 90) for _ in range(5)]); put('dec', 'nan')
for k in ('PSTR', 'PPBR', 'PDTR', 'PDBR', 'PSVR', 'PU2R', 'PHPC', 'PXYZ'):
    put('smc', k, *[r.uniform(0, 40000) for _ in range(N)])
put('rpm', 0, 5349.0, *[float(round(r.uniform(1300, 5000))) for _ in range(N)])
put('rpm', 1, 5777.0, *[0.0 for _ in range(N)])
names = [f'SMC P Tp0{i}' for i in range(8)] + [f'SMC E Tp1{i}' for i in range(4)] + ['SMC G Tg0f', 'SMC G Tg0j', 'PMU tdie1',
         'PMU tdie2', 'NAND CH0 temp', 'gas gauge battery', 'PMU tdev1']
for _ in range(N):
    put('temps', '|'.join(f'{n}={r.uniform(35, 99)!r}' for n in names))
# frame 2: one more sample everywhere; frame 3: a smaller terminal
put('push', r.uniform(0, 5000), r.uniform(0, 60000), r.uniform(0, 20000), float(round(r.uniform(1300, 5000))))
print('\n'.join(L))
