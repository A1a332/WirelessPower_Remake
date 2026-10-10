import re, sys, json

FILES = [
    r"F:/Workbuddy/SMLDev/SMLStarterProject/Mods/WirelessPower/Source/WirelessPower/Registration.cpp",
    r"F:/Workbuddy/SMLDev/SMLStarterProject/Mods/WirelessPower/Source/WirelessPower/WirelessPowerPanelWidget.cpp",
]

def read_text_arg(s, i):
    # s[i] should be 'T' of TEXT( ... returns (decoded_string, index_after_close_paren)
    # find first '"'
    q = s.find('"', i)
    if q == -1:
        return None, i
    buf = []
    j = q + 1
    while j < len(s):
        c = s[j]
        if c == '\\' and j + 1 < len(s):
            nxt = s[j+1]
            if nxt == 'n': buf.append('\n')
            elif nxt == 't': buf.append('\t')
            elif nxt == 'r': buf.append('\r')
            elif nxt == '"': buf.append('"')
            elif nxt == '\\': buf.append('\\')
            else: buf.append(nxt)
            j += 2
            continue
        if c == '"':
            break
        buf.append(c)
        j += 1
    # now j at closing quote; find matching close paren
    p = s.find(')', j)
    return ''.join(buf), p + 1

def extract_wp_text(s):
    out = []
    idx = 0
    while True:
        pos = s.find('WP_Text(', idx)
        if pos == -1:
            break
        # parse first arg
        i = pos + len('WP_Text(')
        # skip whitespace
        while i < len(s) and s[i] in ' \t\r\n':
            i += 1
        if not s.startswith('TEXT(', i):
            idx = i
            continue
        en, i = read_text_arg(s, i)
        # skip whitespace and comma
        while i < len(s) and s[i] in ' \t\r\n,':
            i += 1
        if not s.startswith('TEXT(', i):
            idx = i
            continue
        zh, i = read_text_arg(s, i)
        out.append((en, zh))
        idx = i
    return out

allpairs = []
seen = set()
for f in FILES:
    with open(f, 'r', encoding='utf-8-sig', errors='replace') as fh:
        s = fh.read()
    pairs = extract_wp_text(s)
    for en, zh in pairs:
        if en not in seen:
            seen.add(en)
            allpairs.append((en, zh))

with open(r"F:/Workbuddy/SMLDev/SMLStarterProject/Mods/WirelessPower/_strings.json", 'w', encoding='utf-8') as fh:
    json.dump([{"en": e, "zh": z} for e, z in allpairs], fh, ensure_ascii=False, indent=1)

print(f"TOTAL UNIQUE: {len(allpairs)}")
for n, (e, z) in enumerate(allpairs, 1):
    print(f"[{n}] EN={e!r}")
    print(f"    ZH={z!r}")
