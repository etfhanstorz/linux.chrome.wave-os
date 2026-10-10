#!/usr/bin/env python3
"""Save a real web page and its style sheets (and pictures) for the fake Chromebook, so the browser can be tested on real sites offline.

    python3 tools/grab_site.py NAME URL

Fetches URL the way wave-os does (User-Agent: wave-os, HTTP/1.0 style headers), then up to 10 <link rel=stylesheet> files and 16 <img>
pictures, into fakehana/sites/NAME/ with routes.json (host+path -> file). fakehana serves every saved site under its real names.
The saved copies are for local testing only: fakehana/sites/ is not part of the repository."""
import json, os, re, sys, urllib.parse, urllib.request

HDR = {'User-Agent': 'wave-os', 'Accept': 'text/html, text/plain, */*'}

def get(url):
    req = urllib.request.Request(url, headers=HDR)
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.geturl(), r.headers.get('Content-Type', ''), r.read()

def main():
    name, url = sys.argv[1], sys.argv[2]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fakehana', 'sites', name)
    os.makedirs(out, exist_ok=True)
    routes = {}; count = [0]
    def save(u, ctype, data):
        p = urllib.parse.urlsplit(u)
        key = p.hostname + (p.path or '/') + ('?' + p.query if p.query else '')
        if key in routes: return                                      # listed twice: saved once
        fn = '%03d' % count[0]; count[0] += 1
        open(os.path.join(out, fn), 'wb').write(data)
        routes[key] = {'file': fn, 'type': ctype}
        print('%7d  %s' % (len(data), key))
    final, ctype, html = get(url)
    if final != url:
        save(url, 'redirect:' + final, b'')
    save(final, ctype, html)
    text = html.decode('utf-8', 'replace')
    links = []
    for m in re.finditer(r'<link\b[^>]*>', text, re.I):
        tag = m.group(0)
        if re.search(r'stylesheet', tag, re.I):
            h = re.search(r'(?<![\w-])href\s*=\s*["\']?([^"\'\s>]+)', tag, re.I)
            if h: links.append(h.group(1).replace('&amp;', '&'))
    imgs = []
    for m in re.finditer(r'<img\b[^>]*>', text, re.I):
        tag = m.group(0)
        h = re.search(r'\sdata-src\s*=\s*["\']([^"\']+)', tag, re.I) or re.search(r'\ssrc\s*=\s*["\']([^"\']+)', tag, re.I)
        if h and not h.group(1).startswith('data:'): imgs.append(h.group(1).replace('&amp;', '&'))
    for href in links[:24] + imgs[:16]:
        u = urllib.parse.urljoin(final, href)
        if not u.startswith('http'): continue
        try:
            fu, ct, data = get(u)
            save(u, ct, data)
        except Exception as e:
            print('   skip %s (%s)' % (u, e))
    json.dump(routes, open(os.path.join(out, 'routes.json'), 'w'), indent=1)

main()
