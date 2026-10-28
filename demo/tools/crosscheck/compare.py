"""Compare printer.cpp's output with run.mjs's. Called by crosscheck.sh.

    compare.py exact A B   every line identical; exit 1 and show the first few if not
    compare.py ulps  A B   the same structure, and how far apart the numbers are:
                           doubles (16 hex digits) and floats (8) in units in the
                           last place, the tables' floats counted on their own,
                           since they are what the GPU reads. Exit 1 only if the
                           structure differs (a line that is not the same KIND
                           of line, or a count that differs).
    compare.py gpu   A B   as ulps, and exit 1 if anything the GPU reads differs
                           at all: a table float, a row's float warp, a uniform
                           (`u` lines), or a declaration (`decl`: the panel).

Each line is a kind (its first word, or `u <pass> <name>` for a uniform)
followed by tokens. Hex tokens are numbers; anything else must match exactly.
"""
import struct
import sys


def lines(path):
    with open(path) as handle:
        return handle.read().splitlines()


def exact(a, b):
    left, right = lines(a), lines(b)
    shown = 0
    bad = 0
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            bad += 1
            if shown < 6:
                print(f"line {i + 1}:\n  C++: {x[:160]}\n  js : {y[:160]}")
                shown += 1
    if bad:
        print(f"{bad} of {max(len(left), len(right))} lines differ")
        return 1
    print(f"{len(left)} lines identical")
    return 0


def is_hex(token, digits):
    if len(token) != digits:
        return False
    try:
        int(token, 16)
        return True
    except ValueError:
        return False


def ulps(x, y, digits):
    if digits == 16:
        a = struct.unpack(">q", bytes.fromhex(x))[0]
        b = struct.unpack(">q", bytes.fromhex(y))[0]
        a = a if a >= 0 else -(1 << 63) - a
        b = b if b >= 0 else -(1 << 63) - b
    else:
        a = struct.unpack(">i", bytes.fromhex(x))[0]
        b = struct.unpack(">i", bytes.fromhex(y))[0]
        a = a if a >= 0 else -(1 << 31) - a
        b = b if b >= 0 else -(1 << 31) - b
    return abs(a - b)


def kind_of(tokens):
    if not tokens:
        return ""
    if tokens[0] == "u":
        return " ".join(tokens[:3])
    return tokens[0]


def report(a, b, gpu=False):
    left, right = lines(a), lines(b)
    if len(left) != len(right):
        print(f"STRUCTURE: {len(left)} lines against {len(right)}")
        return 1
    stats = {}
    table_floats = table_differ = table_worst = 0
    for i, (x, y) in enumerate(zip(left, right)):
        tx, ty = x.split(), y.split()
        kx = kind_of(tx)
        if kx != kind_of(ty) or len(tx) != len(ty):
            print(f"STRUCTURE at line {i + 1}:\n  C++: {x[:160]}\n  js : {y[:160]}")
            return 1
        if kx == "data":
            for p, q in zip(tx[3:], ty[3:]):
                table_floats += 1
                if p != q:
                    table_differ += 1
                    table_worst = max(table_worst, ulps(p, q, 8))
            continue
        s = stats.setdefault(kx, {"lines": 0, "differ": 0, "double": 0, "float": 0})
        s["lines"] += 1
        if x == y:
            continue
        s["differ"] += 1
        for j, (p, q) in enumerate(zip(tx, ty)):
            if p == q:
                continue
            if kx == "row" and tx[j - 1] == "hash":
                continue  # follows from the data lines
            for digits, name in ((16, "double"), (8, "float")):
                if is_hex(p, digits) and is_hex(q, digits):
                    s[name] = max(s[name], ulps(p, q, digits))
                    break
            else:
                print(f"STRUCTURE at line {i + 1}, a token that is not a number differs:\n  C++: {x[:160]}\n  js : {y[:160]}")
                return 1
    print(f"tables: {table_differ:,} of {table_floats:,} floats differ"
          + (f", worst {table_worst} ulp" if table_differ else ""))
    for kind, s in sorted(stats.items()):
        if s["differ"]:
            worst = ", ".join(f"{s[n]} {n} ulp" for n in ("float", "double") if s[n])
            print(f"{kind}: {s['differ']} of {s['lines']} lines differ, worst {worst}")
    if not any(s["differ"] for s in stats.values()) and not table_differ:
        print("every line identical")
    if gpu:
        facing = [k for k, s in stats.items() if s["differ"] and (k.startswith("u ") or k == "decl")]
        if table_differ or facing or stats.get("row", {}).get("float", 0):
            print("FAIL something the GPU reads differs: " + ", ".join(facing + (["tables"] if table_differ else [])
                                                                       + (["row warps"] if stats.get("row", {}).get("float", 0) else [])))
            return 1
    return 0


def main(argv):
    if len(argv) != 3 or argv[0] not in ("exact", "ulps", "gpu"):
        print(__doc__)
        return 2
    if argv[0] == "exact":
        return exact(argv[1], argv[2])
    return report(argv[1], argv[2], gpu=argv[0] == "gpu")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
