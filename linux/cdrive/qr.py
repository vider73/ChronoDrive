"""A small QR code encoder (byte mode, error correction M, versions 1-40) and a terminal renderer, standard library
only: the pairing link is shown as a QR the phone scans. After Project Nayuki's reference implementation (MIT)."""

# error correction level M
_ECC_PER_BLOCK = (-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28,
                  28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28)
_BLOCKS = (-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31,
           33, 35, 37, 38, 40, 43, 45, 47, 49)
_ECL_FORMAT = 0   # M


def _raw_modules(ver):
    r = (16 * ver + 128) * ver + 64
    if ver >= 2:
        n = ver // 7 + 2
        r -= (25 * n - 10) * n - 55
        if ver >= 7:
            r -= 36
    return r


def _data_codewords(ver):
    return _raw_modules(ver) // 8 - _ECC_PER_BLOCK[ver] * _BLOCKS[ver]


def _gf_mul(x, y):
    z = 0
    for i in reversed(range(8)):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z


def _rs_divisor(degree):
    res = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            res[j] = _gf_mul(res[j], root)
            if j + 1 < degree:
                res[j] ^= res[j + 1]
        root = _gf_mul(root, 0x02)
    return res


def _rs_remainder(data, div):
    res = [0] * len(div)
    for b in data:
        factor = b ^ res.pop(0)
        res.append(0)
        for i, c in enumerate(div):
            res[i] ^= _gf_mul(c, factor)
    return res


class _Qr:
    def __init__(self, ver, codewords):
        self.ver, self.size = ver, ver * 4 + 17
        n = self.size
        self.m = [[False] * n for _ in range(n)]
        self.fn = [[False] * n for _ in range(n)]
        self._function_patterns()
        self._draw_codewords(self._add_ecc(codewords))
        best, best_pen = 0, None
        for mask in range(8):
            self._mask(mask)
            self._format_bits(mask)
            p = self._penalty()
            if best_pen is None or p < best_pen:
                best, best_pen = mask, p
            self._mask(mask)   # undo (xor)
        self._mask(best)
        self._format_bits(best)

    def _set(self, x, y, dark):
        self.m[y][x] = dark
        self.fn[y][x] = True

    def _function_patterns(self):
        n = self.size
        for i in range(n):
            self._set(6, i, i % 2 == 0)
            self._set(i, 6, i % 2 == 0)
        for cx, cy in ((3, 3), (n - 4, 3), (3, n - 4)):
            for dy in range(-4, 5):
                for dx in range(-4, 5):
                    x, y = cx + dx, cy + dy
                    if 0 <= x < n and 0 <= y < n:
                        self._set(x, y, max(abs(dx), abs(dy)) not in (2, 4))
        pos = self._align_positions()
        last = len(pos) - 1
        for i, a in enumerate(pos):
            for j, b in enumerate(pos):
                if (i == 0 and j == 0) or (i == 0 and j == last) or (i == last and j == 0):
                    continue
                for dy in range(-2, 3):
                    for dx in range(-2, 3):
                        self._set(a + dx, b + dy, max(abs(dx), abs(dy)) != 1)
        self._format_bits(0)
        if self.ver >= 7:
            rem = self.ver
            for _ in range(12):
                rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
            bits = self.ver << 12 | rem
            for i in range(18):
                bit = (bits >> i) & 1 == 1
                a, b = n - 11 + i % 3, i // 3
                self._set(a, b, bit)
                self._set(b, a, bit)

    def _align_positions(self):
        if self.ver == 1:
            return []
        num = self.ver // 7 + 2
        step = (self.ver * 8 + num * 3 + 5) // (num * 4 - 4) * 2
        return [6] + list(reversed([self.size - 7 - i * step for i in range(num - 1)]))

    def _format_bits(self, mask):
        data = _ECL_FORMAT << 3 | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        bits = (data << 10 | rem) ^ 0x5412
        n = self.size
        b = lambda i: (bits >> i) & 1 == 1   # noqa: E731
        for i in range(6):
            self._set(8, i, b(i))
        self._set(8, 7, b(6))
        self._set(8, 8, b(7))
        self._set(7, 8, b(8))
        for i in range(9, 15):
            self._set(14 - i, 8, b(i))
        for i in range(8):
            self._set(n - 1 - i, 8, b(i))
        for i in range(8, 15):
            self._set(8, n - 15 + i, b(i))
        self._set(8, n - 8, True)

    def _add_ecc(self, data):
        nblocks, ecclen = _BLOCKS[self.ver], _ECC_PER_BLOCK[self.ver]
        raw = _raw_modules(self.ver) // 8
        nshort = nblocks - raw % nblocks
        shortlen = raw // nblocks
        div = _rs_divisor(ecclen)
        blocks, k = [], 0
        for i in range(nblocks):
            dat = data[k:k + shortlen - ecclen + (0 if i < nshort else 1)]
            k += len(dat)
            ecc = _rs_remainder(dat, div)
            if i < nshort:
                dat = dat + [0]
            blocks.append(dat + ecc)
        out = []
        for i in range(len(blocks[0])):
            for j, blk in enumerate(blocks):
                if i != shortlen - ecclen or j >= nshort:
                    out.append(blk[i])
        return out

    def _draw_codewords(self, data):
        n, i = self.size, 0
        right = n - 1
        while right >= 1:
            if right == 6:
                right = 5
            for vert in range(n):
                for j in range(2):
                    x = right - j
                    up = ((right + 1) & 2) == 0
                    y = n - 1 - vert if up else vert
                    if not self.fn[y][x] and i < len(data) * 8:
                        self.m[y][x] = (data[i >> 3] >> (7 - (i & 7))) & 1 == 1
                        i += 1
            right -= 2

    def _mask(self, mask):
        f = (lambda x, y: (x + y) % 2 == 0, lambda x, y: y % 2 == 0, lambda x, y: x % 3 == 0, lambda x, y: (x + y) % 3 == 0,
             lambda x, y: (x // 3 + y // 2) % 2 == 0, lambda x, y: x * y % 2 + x * y % 3 == 0,
             lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0, lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0)[mask]
        for y in range(self.size):
            for x in range(self.size):
                if not self.fn[y][x] and f(x, y):
                    self.m[y][x] = not self.m[y][x]

    def _penalty(self):
        """Rules 1 (runs), 2 (2x2 blocks) and 4 (balance); any mask decodes, this only picks a clean-looking one."""
        n, m, p = self.size, self.m, 0
        for lines in (m, [list(c) for c in zip(*m)]):
            for row in lines:
                run, prev = 0, None
                for v in row:
                    if v == prev:
                        run += 1
                        if run == 5:
                            p += 3
                        elif run > 5:
                            p += 1
                    else:
                        run, prev = 1, v
        for y in range(n - 1):
            for x in range(n - 1):
                if m[y][x] == m[y][x + 1] == m[y + 1][x] == m[y + 1][x + 1]:
                    p += 3
        dark = sum(sum(r) for r in m)
        p += ((abs(dark * 20 - n * n * 10) + n * n - 1) // (n * n) - 1) * 10
        return p


def encode(text):
    """The QR matrix (rows of booleans, True = dark) of a text, in byte mode."""
    data = text.encode("utf-8")
    for ver in range(1, 41):
        cc = 8 if ver < 10 else 16
        cap = _data_codewords(ver) * 8
        need = 4 + cc + len(data) * 8
        if need <= cap:
            break
    else:
        raise ValueError("text too long for a QR code")
    bits = []

    def put(v, n):
        bits.extend((v >> i) & 1 for i in reversed(range(n)))

    put(0b0100, 4)
    put(len(data), cc)
    for b in data:
        put(b, 8)
    put(0, min(4, cap - len(bits)))
    put(0, (-len(bits)) % 8)
    pad = 0xEC
    while len(bits) < cap:
        put(pad, 8)
        pad ^= 0xEC ^ 0x11
    words = [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    return _Qr(ver, words).m


def render(text, quiet=4):
    """The QR as terminal lines: two modules per character cell (upper half block), black on white with explicit
    colours so it scans on dark and light terminals alike."""
    m = encode(text)
    n = len(m)
    full = [[False] * (n + 2 * quiet) for _ in range(quiet)] + [[False] * quiet + r + [False] * quiet for r in m] + \
        [[False] * (n + 2 * quiet) for _ in range(quiet + 1)]
    lines = []
    for y in range(0, n + 2 * quiet, 2):
        top, bot = full[y], full[y + 1]
        s = []
        for a, b in zip(top, bot):
            fg = "30" if a else "97"   # the upper module
            bg = "40" if b else "107"  # the lower module
            s.append(f"\x1b[{fg};{bg}m▀")
        lines.append("".join(s) + "\x1b[0m")
    return lines
