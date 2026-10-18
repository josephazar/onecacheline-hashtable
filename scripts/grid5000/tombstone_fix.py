#!/usr/bin/env python3
"""make every probe terminate after deletions.

The chunked tables and the separate-array baseline rehashed only when the number of LIVE keys reached the max load.
Erase writes a tombstone (DELETED) in a chunk without an EMPTY slot, so under delete/insert churn the EMPTY slots can
all disappear while the live load stays below the threshold; a lookup for an absent key whose probe meets no EMPTY
slot then never stops (a four-chunk trace reproduces this, see tombstone_test.cc).

The patch, the rule Abseil uses: count the non-EMPTY slots (live + tombstones) in `used`; insert consumes an EMPTY
slot -> used++; erase that writes EMPTY -> used--, one that writes DELETED leaves it; before an insert, if `used`
reached the max load, rebuild the table at the same capacity (purges tombstones, recomputes every overflow filter)
or, when at least 7/8 of those slots hold live keys, double it.  Hence used <= max load < capacity: at least one slot,
so one chunk, is always EMPTY and every probe ends within one traversal of the table.  Lookups are untouched.

Usage:  python3 tombstone_fix.py header.hpp [...]      (patches in place; idempotent; prints what it changed)
Works on swiss_table_chunk.hpp (Detached), swiss_table_chunkv.hpp (Inline), swiss_table_chunkvf.hpp (Inline+F),
swiss_table_chunk2.hpp (Paired) and swiss_table_h1h2.hpp (separate-array SoA), in the orig/pf2/pf3 bundles alike.
"""
import re
import sys

TAG = 'tombstone count'


def patch(path):
    s = open(path).read()
    if TAG in s:
        print(f'{path}: already patched'); return True
    n = {}
    # 1. counters next to `size`, public so the tests can read them, and a same-capacity rebuild entry point
    m = re.search(r'rehash\((num_chunks|capacity) \* 2\);', s); cap = m.group(1) if m else 'num_chunks'
    s, n['decl'] = re.subn(r'(\n([ \t]*)size_t[ \t]+size;)',
                           r'\1\n\2public: size_t used = 0, n_rebuilds = 0, n_grows = 0;   // ' + TAG +
                           r': non-EMPTY slots (live + DELETED), rebuild counters\n\2void rehash_same() { rehash(' + cap +
                           r'); }   // rebuild at the same capacity (tests, timing)\n\2private:', s, count=1)
    # 2. rehash resets the count (insert re-counts every surviving key)
    s, n['reset'] = re.subn(r'\bsize = 0;', 'size = 0; used = 0;', s, count=1)
    # 3. trigger on used, rebuild in place unless mostly live
    s, n['trigger'] = re.subn(
        r'if \(size >= ([^()]+?)\) rehash\((num_chunks|capacity) \* 2\);',
        lambda m: (f'if (used >= {m.group(1)}) {{   // {TAG}: live + tombstones reached the max load\n'
                   f'            if (size >= ({m.group(1)}) * 0.875) {{ ++n_grows; rehash({m.group(2)} * 2); }}\n'
                   f'            else {{ ++n_rebuilds; rehash({m.group(2)}); }}   // same capacity: tombstones purged, filters recomputed\n'
                   f'        }}'), s, count=1)
    # 4. insert: an EMPTY slot consumed raises `used` (a reused DELETED slot does not)
    s, n['insert'] = re.subn(r'((ch\.ctrl\[b\]|control_bytes\[s\])\s*=(?!=)\s*sig;)',
                             r'used += (\2 == EMPTY); \1', s, count=1)
    # 5. erase: writing EMPTY gives the slot back, writing DELETED does not
    s, n['erase'] = re.subn(r'(([\w.]+\[\w+\])\s*=(?!=)\s*(?:\(\s*\w+\s*!=\s*0\s*\)|\w+)\s*\?\s*EMPTY\s*:\s*DELETED;)',
                            r'\1 if (\2 == EMPTY) used--;', s, count=1)
    ok = all(v == 1 for v in n.values())
    if ok:
        open(path, 'w').write(s)
    print(f'{path}: ' + ', '.join(f'{k}={v}' for k, v in n.items()) + ('' if ok else '  -> NOT patched (pattern missing)'))
    return ok


if __name__ == '__main__':
    sys.exit(0 if all([patch(p) for p in sys.argv[1:]]) else 1)
