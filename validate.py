"""Check the solved PA->CHA hash against independently sampled files ("pa cha max second" lines)."""
import sys

MASK = [0x5B5F575440, 0x6EB5FAA880, 0x7CCCC93100, 0x71AEEB1200]
NODE1_BASE = 0x2080000000  # SRAT: node0 [0, 0x207fffffff], node1 [0x2080000000, 0x407fffffff]


def cha_of(pa):
    h = sum((bin(pa & m).count("1") & 1) << b for b, m in enumerate(MASK))
    return (16 if pa >= NODE1_BASE else 0) + h


for path in sys.argv[1:]:
    n = ok = clean = clean_ok = 0
    bad = []
    for line in open(path):
        a, c, bv, sv = line.split()
        pa, c, bv, sv = int(a, 16), int(c), int(bv), int(sv)
        hit = cha_of(pa) == c
        n += 1
        ok += hit
        if bv >= 90 and sv <= 30:  # same cleanliness filter as solve.py
            clean += 1
            clean_ok += hit
        if not hit:
            bad.append(f"    {a} measured={c} max={bv} second={sv} predicted={cha_of(pa)}")
    print(f"{path}: all {ok}/{n}, clean {clean_ok}/{clean}")
    print("\n".join(bad[:10]))
