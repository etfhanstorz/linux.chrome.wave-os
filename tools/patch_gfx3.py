# v1.10-001: splice the two-pass layout (flex, grid, tables, inline-block) into gfx.h; the browser forgets the old page's layout data
root = '/mnt/c/!ab1/os/'
s = open(root + 'gfx.h', newline='').read().replace('\r\n', '\n')
a = s.index('// ---------------- layout ----------------')
b = s.index("// Collect the page's style sheets")
s = s[:a] + open(root + 'tools/gfx_layout.c.txt').read().replace('\r\n', '\n') + s[b:]
# gm is declared in the layout section; g_collect_css (after it) clears it
open(root + 'gfx.h', 'w', newline='').write(s)

w = open(root + 'web.h', newline='').read().replace('\r\n', '\n')
old = '''    web_page_gfx = 0;
    for (int hops = 0; hops < 6; hops++) {'''
assert old in w
w = w.replace(old, '''    web_page_gfx = 0; gm = 0; ncrules = 0; crule_next = 0;            // the old page's layout data lived in picture memory
    for (int hops = 0; hops < 6; hops++) {''', 1)
open(root + 'web.h', 'w', newline='').write(w)
print('ok')
