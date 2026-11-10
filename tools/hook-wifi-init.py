from pathlib import Path
import sys

p = Path('xnu/fuxi-h1/fuxi_keys.c')
lines = p.read_text().splitlines(True)

# 1) prototype next to the existing one
for i, line in enumerate(lines):
    if line.strip() == 'void fuxi_keys_init(void);':
        lines.insert(i + 1, 'void fuxi_wifi_probe_init(void); /* bsd/dev/fuxi_wifi.c: stage 1, fuxiwifiprobe */\n')
        break
else:
    sys.exit('prototype anchor missing')

# 2) call at the end of fuxi_keys_init, before its closing brace
start = None
for i, line in enumerate(lines):
    if line.startswith('fuxi_keys_init(void)'):
        start = i
        break
if start is None:
    sys.exit('function anchor missing')

depth = 0
seen_body = False
end = None
for i in range(start, len(lines)):
    depth += lines[i].count('{') - lines[i].count('}')
    if lines[i].count('{'):
        seen_body = True
    if seen_body and depth == 0:
        end = i
        break
if end is None:
    sys.exit('closing brace missing')
if 'fuxi_wifi_probe_init();' in ''.join(lines[start:end]):
    sys.exit('already hooked')

lines.insert(end, '\tfuxi_wifi_probe_init();\n')
p.write_bytes(''.join(lines).replace('\r\n', '\n').encode())
print('hooked fuxi_wifi_probe_init at line', end + 1)