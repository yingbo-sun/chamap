import sys
import numpy as np

LO, HI = 6, 46  # PA bits considered


def load(path):
    pa, cha = [], []
    for line in open(path):
        a, c, bv, sv = line.split()
        if int(bv) >= 90 and int(sv) <= 30:
            pa.append(int(a, 16))
            cha.append(int(c))
    return np.array(pa, dtype=np.uint64), np.array(cha)


def bits_matrix(pa):
    cols = [((pa >> np.uint64(b)) & np.uint64(1)).astype(np.uint8) for b in range(LO, HI)]
    cols.append(np.ones(len(pa), dtype=np.uint8))  # affine constant
    return np.stack(cols, axis=1)


def gf2_solve(A, y):
    """Least-structure solve A x = y over GF(2); returns x (free vars = 0) or None if inconsistent."""
    A = A.copy()
    y = y.copy()
    n, m = A.shape
    piv_cols = []
    r = 0
    for c in range(m):
        rows = np.nonzero(A[r:, c])[0]
        if len(rows) == 0:
            continue
        p = r + rows[0]
        A[[r, p]] = A[[p, r]]
        y[[r, p]] = y[[p, r]]
        mask = A[:, c].astype(bool)
        mask[r] = False
        A[mask] ^= A[r]
        y[mask] ^= y[r]
        piv_cols.append(c)
        r += 1
        if r == n:
            break
    if np.any(y[r:]):
        return None, piv_cols
    x = np.zeros(m, dtype=np.uint8)
    for i, c in enumerate(piv_cols):
        x[c] = y[i]
    return x, piv_cols


for path in sys.argv[1:]:
    pa, cha = load(path)
    A = bits_matrix(pa)
    local = cha & 15
    print(f"{path}: {len(pa)} clean samples, CHAs {sorted(set(cha))}")
    x_all = []
    for ob in range(4):
        y = ((local >> ob) & 1).astype(np.uint8)
        x, piv = gf2_solve(A, y)
        if x is None:
            print(f"  out bit {ob}: INCONSISTENT (not affine in PA[{LO}:{HI}])")
            continue
        used = [LO + i for i in range(HI - LO) if x[i]]
        mask = sum(1 << b for b in used)
        print(f"  out bit {ob}: const={x[-1]} mask={mask:#014x} bits={used}")
        x_all.append(x)
    if len(x_all) == 4:
        pred = sum(((A @ x) % 2).astype(int) << ob for ob, x in enumerate(x_all))
        print(f"  prediction accuracy: {np.mean(pred == local):.6f}")
    unknown = [LO + c for c in range(HI - LO) if c not in piv]
    print(f"  PA bits not determined by data (no variation): {unknown}")
