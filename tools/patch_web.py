root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

# entity length: return how many characters after the '&' were used
edit('web.h', [
("""        if (i < left && s[i] == ';') i++;
        const char *r = v >= 32 && v < 127 ? 0 : web_cp(v);
        if (!r) { out[0] = (char)v; out[1] = 0; } else { u32 k = 0; while (r[k] && k < 6) { out[k] = r[k]; k++; } out[k] = 0; }
        return i + 1;""",
"""        if (i < left && s[i] == ';') i++;
        const char *r = v >= 32 && v < 127 ? 0 : web_cp(v);
        if (!r) { out[0] = (char)v; out[1] = 0; } else { u32 k = 0; while (r[k] && k < 6) { out[k] = r[k]; k++; } out[k] = 0; }
        return i;"""),
("""        if (!tab[t].n[k]) { u32 j = 0; while (tab[t].v[j]) { out[j] = tab[t].v[j]; j++; } out[j] = 0; return k + 1; }""",
"""        if (!tab[t].n[k]) { u32 j = 0; while (tab[t].v[j]) { out[j] = tab[t].v[j]; j++; } out[j] = 0; return k; }"""),
("""                            if (used) { for (u32 e = 0; ent[e] && v + 1 < dmax; e++) dst[v++] = ent[e]; j += used; continue; }""",
"""                            if (used) { for (u32 e = 0; ent[e] && v + 1 < dmax; e++) dst[v++] = ent[e]; j += used + 1; continue; }"""),
])

edit('main.c', [("""#include "wpa.h\"""", """#include "wpa.h"
#include "web.h\"""")])

edit('shell.h', [
("""    else if (streq(line, "dns")) wifi_dns(arg);""",
"""    else if (streq(line, "dns")) wifi_dns(arg);
    else if (streq(line, "web")) web_run(*arg ? arg : "http://example.com");"""),
("""    else if (streq(line, "f")) {""",
"""    else if (streq(line, "w")) { line = "web"; }
    else if (streq(line, "f")) {"""),
("""commands: c f j k r up""", """commands: c f j k w r web up"""),
])

# the fake web server: a few test pages on the fake PC (example.com / example.test resolve to it)
edit('fakehana/fakehana.py', [
("""            self.msdc.loader.lan = self.net            # the fake Wi-Fi router forwards to the same fake LAN""",
"""            self.msdc.loader.lan = self.net            # the fake Wi-Fi router forwards to the same fake LAN
        page = (b'<!DOCTYPE html><html><head><title>Fake &amp; Test Page</title><style>body{color:red}</style>'
                b'<script>alert("never shown")</script></head><body><h1>Hello from the fake web</h1>'
                b'<p>This is a <b>test page</b> with   lots   of  spaces, an entity &lt;tag&gt; and &#169; 2026 \\xe2\\x80\\x94 dash.</p>'
                b'<ul><li>First item</li><li>Second item with a <a href="/two.html">link to page two</a></li>'
                b'<li><a href="http://example.test/old">a redirect</a></li><li><a href="https://secure.test/">an https link</a></li></ul>'
                b'<p>' + b'A long paragraph that needs wrapping. ' * 12 + b'</p>'
                b'<pre>  preformatted\\n    keeps   its spaces</pre><img src="x.png" alt="a picture">'
                + b''.join(b'<p>Line %d of filler so the page scrolls.</p>' % i for i in range(1, 41)) + b'</body></html>')
        self.net.web['/'] = (200, '', page)
        self.net.web['/two.html'] = (200, '', b'<html><head><title>Page Two</title></head><body><h2>Page two</h2><p>You followed a link. <a href="/">Back to the start</a></p></body></html>')
        self.net.web['/old'] = (302, 'Location: /two.html\\r\\n', b'moved')"""),
])
print('ok')
