# Prints the function tables of d3d-api-map.md from the d3d_api.json draft, grouped.
import json, sys
d = json.load(open(sys.argv[1]))
groups = [('device', 'Device'), ('resource', 'Resources (common)'), ('buffer', 'Vertex and index buffers'),
          ('texture', 'Textures'), ('surface', 'Surfaces'), ('shader', 'Shaders'), ('declaration', 'Vertex declarations'),
          ('constants', 'Shader constants'), ('state', 'Other state'), ('query', 'Occlusion queries and conditional rendering'),
          ('draw', 'Draw'), ('clear_resolve', 'Clear and resolve'), ('present', 'Present'), ('stateblock', 'State blocks'),
          ('cpu_helper', 'CPU helpers (keep)'), ('shader_assembler', 'Shader microcode assembler (keep)'),
          ('d3dx', 'D3DX (keep)'), ('xboxmath', 'xboxmath (keep)'), ('xapi', 'XAPI, not Direct3D (keep)')]
scene_key = sys.argv[2] if len(sys.argv) > 2 else 'vrex_gameplay_all_states'
def args(f):
    return ', '.join(a['name'] + (' (' + a['reg'] + ')' if a['reg'] not in ('',) else '') for a in f['args'])
for g, title in groups:
    fs = [f for f in d['functions'] if f['group'] == g]
    if not fs:
        continue
    print('### ' + title)
    print()
    if g in ('shader_assembler', 'xboxmath', 'xapi'):
        print(', '.join('`%s` %s' % (f['sub'], f['name']) for f in fs) + '.')
        print()
        continue
    print('| Function | Name | Arguments | Returns | Native | Per frame | Notes |')
    print('|---|---|---|---|---|---|---|')
    for f in fs:
        pf = f['per_frame'].get(scene_key, '')
        print(f"| `{f['sub']}` | {f['name']} | {args(f)} | {f['returns']} | {f['renderer']} | {pf} | {f['notes'].replace('|', chr(92) + '|')} |")
    print()
