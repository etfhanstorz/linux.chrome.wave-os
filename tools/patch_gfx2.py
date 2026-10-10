# v1.10: splice the new g_open / g_close_top (box model) into gfx.h, plus layout setup changes
root = '/mnt/c/!ab1/os/'
s = open(root + 'gfx.h', newline='').read().replace('\r\n', '\n')
a = s.index('static void g_open(')
b = s.index('static void g_close(const char *tn) {')
s = s[:a] + open(root + 'tools/gfx_open.c.txt').read().replace('\r\n', '\n') + s[b:]

def rep(old, new):
    global s
    assert old in s, old[:70]
    s = s.replace(old, new, 1)

# page setup: Chrome's 16 px text, the page starts at the edge (body has the 8 px margin)
rep('''    g_left0 = 24; g_right0 = width - 24;''', '''    g_left0 = 0; g_right0 = width;''')
rep('''    s->size = 15; s->bold = 0; s->mono = 0; s->pre = 0; s->align = 0; s->hidden = 0; s->under = 0; s->block = 1; s->color = 0x111111; s->bg = 0xffffff;''',
    '''    s->size = 16; s->bold = 0; s->mono = 0; s->pre = 0; s->align = 0; s->hidden = 0; s->under = 0; s->block = 1; s->color = 0x000000; s->bg = 0xffffff; s->listnone = 0; s->nowrap = 0; s->imgw = s->imgh = 0;''')
rep('''    g_x = g_left0; g_y = 16; g_line_start = g_x; g_line_asc = g_line_desc = 0; g_pend_space = 0; g_line_empty = 1; g_line_item = 0; g_last_margin = 16;''',
    '''    g_x = g_left0; g_y = 0; g_line_start = g_x; g_line_asc = g_line_desc = 0; g_pend_space = 0; g_line_empty = 1; g_line_item = 0; g_last_margin = 0; g_line_no = 0;''')
rep('''    root->tag[0] = 0; root->htag = 0; root->hid = 0; root->ncls = 0; root->bgitem = 0xffffffff;''',
    '''    root->tag[0] = 0; root->htag = 0; root->hid = 0; root->ncls = 0; root->bgitem = root->blitem = root->britem = root->ibitem = 0xffffffff; root->pb = root->bb = root->mb = root->minh = 0;''')
rep('''    gpage_h = g_y + 24;''', '''    gpage_h = g_y + 8;''')
# tag attributes: hidden, width, height
rep('''    static char tn[12], idv[40], clsv[160], stylev[400], href[WEB_HREF], alt[60], isrc[WEB_HREF], dsrc[WEB_HREF];''',
    '''    static char tn[12], idv[40], clsv[160], stylev[400], href[WEB_HREF], alt[60], isrc[WEB_HREF], dsrc[WEB_HREF], wv[12], hv[12];''')
rep('''            idv[0] = clsv[0] = stylev[0] = href[0] = alt[0] = isrc[0] = dsrc[0] = 0;''',
    '''            idv[0] = clsv[0] = stylev[0] = href[0] = alt[0] = isrc[0] = dsrc[0] = wv[0] = hv[0] = 0; u32 hidden_attr = 0;''')
rep('''                else if (tag_is(an, "data-src") || tag_is(an, "data-lazy-src") || tag_is(an, "data-original")) { dst = dsrc; dmax = sizeof dsrc; }''',
    '''                else if (tag_is(an, "data-src") || tag_is(an, "data-lazy-src") || tag_is(an, "data-original")) { dst = dsrc; dmax = sizeof dsrc; }
                else if (tag_is(an, "width")) { dst = wv; dmax = sizeof wv; } else if (tag_is(an, "height")) { dst = hv; dmax = sizeof hv; }
                else if (tag_is(an, "hidden")) hidden_attr = 1;''')
rep('''            i = j < n ? j + 1 : n;''',
    '''            i = j < n ? j + 1 : n;
            if (hidden_attr) { const char *dn = "display:none!important"; u32 q = 0; while (dn[q]) { stylev[q] = dn[q]; q++; } stylev[q] = 0; }''')
rep('''                g_open(tn, idv, clsv, stylev, href, 1);
                if (!gstk[gsp].s.hidden) {''',
    '''                g_open(tn, idv, clsv, stylev, href, 1);
                { struct gstyle *is = &gstk[gsp].s; u32 aw = 0, ah = 0;                // width= / height= on the tag (CSS wins)
                  for (u32 q = 0; wv[q] >= '0' && wv[q] <= '9'; q++) aw = aw * 10 + (u32)(wv[q] - '0');
                  for (u32 q = 0; hv[q] >= '0' && hv[q] <= '9'; q++) ah = ah * 10 + (u32)(hv[q] - '0');
                  if (!is->imgw && aw && aw < 4000) is->imgw = (int)aw; if (!is->imgh && ah && ah < 4000 && (!is->imgw || aw)) is->imgh = (int)ah; }
                if (!gstk[gsp].s.hidden) {''')
open(root + 'gfx.h', 'w', newline='').write(s)
print('ok')
