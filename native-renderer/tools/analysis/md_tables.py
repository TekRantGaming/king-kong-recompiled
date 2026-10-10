# Prints the markdown tables of d3d-api-map.md from the d3d_api.json draft.
import json, sys
d = json.load(open(sys.argv[1]))
print('| Index | Value | Name | Setter / getter | Register (device offset) | Bits | Values | Traced |')
print('|---|---|---|---|---|---|---|---|')
for r in d['render_states']:
    if r['name'] is None:
        continue
    note = r['encoding'] + ('; ' + r['note'] if r['note'] else '')
    tv = ', '.join(r['traced_values']) if r['traced_values'] else ''
    print(f"| {r['index']} | {r['value']} | {r['name'][6:]} | `{r['setter']}` / `{r['getter']}` | {r['register']} (+{r['device_offset']}) | {r['bits']} | {note} | {tv} |")
print()
print('| Index | Value | Name | Setter / getter | Fetch word | Bits | Values | Traced |')
print('|---|---|---|---|---|---|---|---|')
for r in d['sampler_states']:
    note = r['encoding'] + ('; ' + r['note'] if r['note'] else '')
    tv = ', '.join(r['traced_values']) if r['traced_values'] else ''
    w = '' if r['fetch_word'] is None else str(r['fetch_word'])
    print(f"| {r['index']} | {r['value']} | {r['name'][8:]} | `{r['setter']}` / `{r['getter']}` | {w} | {r['bits']} | {note} | {tv} |")
