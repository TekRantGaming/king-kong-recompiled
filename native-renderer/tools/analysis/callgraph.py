# Call graph of the recompiled game code: for every function, its direct callees and whether it makes indirect
# calls. Writes callgraph.json next to this script.
# Usage: python callgraph.py <kk/generated/default>
import glob
import json
import os
import re
import sys

gen = sys.argv[1]
func_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8}|[A-Za-z_][A-Za-z0-9_]*)\)')
call_re = re.compile(r'\b((?:__imp__)?sub_[0-9A-F]{8}|__imp__[A-Za-z_][A-Za-z0-9_]*)\(ctx, base\)')
graph = {}
for path in sorted(glob.glob(os.path.join(gen, 'king_kong_recomp.*.cpp'))):
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = func_re.match(line)
            if m:
                cur = m.group(1)
                graph[cur] = {'calls': {}, 'indirect': 0, 'lines': 0}
                continue
            if cur is None:
                continue
            graph[cur]['lines'] += 1
            for c in call_re.findall(line):
                c = c.replace('__imp__', '') if c.startswith('__imp__sub_') else c
                graph[cur]['calls'][c] = graph[cur]['calls'].get(c, 0) + 1
            if 'REX_CALL_INDIRECT_FUNC' in line:
                graph[cur]['indirect'] += 1
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'callgraph.json')
json.dump(graph, open(out, 'w'), indent=0)
print(len(graph), 'functions ->', out)
