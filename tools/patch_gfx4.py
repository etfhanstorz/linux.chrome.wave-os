# v1.10-005: CSS rules share their declarations (a, b, c { ... } is parsed once) so big sites (GitHub: 20000+ selectors) fit
root = '/mnt/c/!ab1/os/'
s = open(root + 'gfx.h', newline='').read().replace('\r\n', '\n')

def rep(old, new, count=1):
    global s
    assert s.count(old) >= count, old[:70]
    s = s.replace(old, new) if count == 0 else s.replace(old, new, count)

rep('struct crule { struct ccomp c[4]; u8 nc; u64 set, imp; u16 spec; u32 order; u32 color, bg, bcolor;',
    'struct cprops { u64 set, imp; u32 color, bg, bcolor;')
rep('''               short radius, lh; u8 tt, mask, flt, clr; };          // flt: 1 left 2 right; clr: 1 left 2 right 3 both''',
    '''               short radius, lh; u8 tt, mask, flt, clr; };          // flt: 1 left 2 right; clr: 1 left 2 right 3 both
struct crule { struct ccomp c[4]; u8 nc; u16 spec; u32 order, p; };         // one selector; p = its declarations (shared by "a, b, c { ... }")''')
rep('static struct crule *crules; static u32 ncrules, crules_max;',
    'static struct crule *crules; static u32 ncrules, crules_max;\nstatic struct cprops *cprops; static u32 ncprops, cprops_max;')
for f in ('static void c_sides(const char *v, u32 n, struct crule *r', 'static void c_border(const char *v, u32 n, struct crule *r',
          'static void c_grid_tracks(const char *v, u32 vn, struct crule *r', 'static void c_decls(const char *d, u32 n, struct crule *r'):
    rep(f, f.replace('struct crule', 'struct cprops'))
rep('static void c_take(struct crule *d, const struct crule *s, u32 b)', 'static void c_take(struct cprops *d, const struct cprops *s, u32 b)')
rep('static void g_plan_grid(struct gnode *e, const struct crule *st, int W, int gap)', 'static void g_plan_grid(struct gnode *e, const struct cprops *st, int W, int gap)')
rep('                    struct crule tmp; tmp.set = 0; tmp.imp = 0; c_decls(s + ds, de - ds, &tmp, 0);',
    '                    struct cprops tmp; tmp.set = 0; tmp.imp = 0; c_decls(s + ds, de - ds, &tmp, 0);')
rep('''        // one rule per comma-separated selector
        for (u32 a = sel; a < sele;) {''', '''        // one rule per comma-separated selector; the declarations are parsed once (when the first selector is usable)
        u32 pidx = 0xffffffff, nodecl = 0;
        for (u32 a = sel; a < sele;) {''')
rep('''                r->nc = 0; r->set = 0; r->imp = 0; r->spec = 0; int ok = 1;''', '''                r->nc = 0; r->spec = 0; int ok = 1;''')
rep('''                if (ok && r->nc) {
                    c_decls(s + ds, de - ds, r, 0);
                    r->order = css_order++;
                    if (r->set) ncrules++;
                }''', '''                if (ok && r->nc && !nodecl) {
                    if (pidx == 0xffffffff) {
                        if (ncprops >= cprops_max) break;
                        struct cprops *pp = &cprops[ncprops]; pp->set = 0; pp->imp = 0;
                        c_decls(s + ds, de - ds, pp, 0);
                        if (!pp->set) { nodecl = 1; a = b + 1; continue; }            // nothing we use: skip all its selectors
                        pidx = ncprops++;
                    }
                    r->p = pidx; r->order = css_order++; ncrules++;
                }''')
# the cascade
rep('    struct crule best;', '    struct cprops best;')
rep('        struct crule pr; pr.set = 0; pr.imp = 0;', '        struct cprops pr; pr.set = 0; pr.imp = 0;')
rep('        struct crule in; in.set = 0; in.imp = 0;', '        struct cprops in; in.set = 0; in.imp = 0;')
rep('''                struct crule *cr = &crules[r];
                if (!c_match(cr, gsp)) continue;
                u64 base = ((u64)cr->spec << 24 | cr->order) + 1;
                for (u64 m = cr->set; m; m &= m - 1) {
                    u32 b = (u32)__builtin_ctzll(m); u64 bit = 1ull << b;
                    u64 rk = base | ((cr->imp & bit) ? 1ull << 42 : 0);
                    if ((have & bit) && rk < rank[b]) continue;
                    have |= bit; rank[b] = rk; c_take(&best, cr, b);''', '''                struct crule *cr = &crules[r];
                if (!c_match(cr, gsp)) continue;
                const struct cprops *cp = &cprops[cr->p];
                u64 base = ((u64)cr->spec << 24 | cr->order) + 1;
                for (u64 m = cp->set; m; m &= m - 1) {
                    u32 b = (u32)__builtin_ctzll(m); u64 bit = 1ull << b;
                    u64 rk = base | ((cp->imp & bit) ? 1ull << 42 : 0);
                    if ((have & bit) && rk < rank[b]) continue;
                    have |= bit; rank[b] = rk; c_take(&best, cp, b);''')
# memory: 40000 selectors, 24000 declaration blocks
rep('''    crules_max = 12000; crules = img_alloc(crules_max * sizeof(struct crule)); crule_next = img_alloc(crules_max * 4);''',
    '''    crules_max = 40000; crules = img_alloc(crules_max * sizeof(struct crule)); crule_next = img_alloc(crules_max * 4);
    ncprops = 0; cprops_max = 24000; cprops = img_alloc(cprops_max * sizeof(struct cprops));''')
rep('''    if (!crules || !crule_next) { crules_max = 0;''', '''    if (!crules || !crule_next || !cprops) { crules_max = 0;''')
rep('''put_dec(ncrules); puts(" rules");''', '''put_dec(ncrules); puts(" rules, "); put_dec(ncprops); puts(" blocks");''')
rep('''      if (ncrules >= crules_max) puts(" (FULL)");''', '''      if (ncrules >= crules_max || ncprops >= cprops_max) puts(" (FULL)");''')
open(root + 'gfx.h', 'w', newline='').write(s)
print('ok')
