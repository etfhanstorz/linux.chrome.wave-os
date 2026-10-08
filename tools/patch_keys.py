root = '/mnt/c/!ab1/os/'

def edit(name, pairs):
    s = open(root + name, newline='').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert old in s, (name, old[:80])
        s = s.replace(old, new, 1)
    open(root + name, 'w', newline='').write(s)

edit('tools/gen_keymap.py', [
("""SHIFT, CTRL, ALT, SEARCH = 1, 2, 3, 4          # modifier markers in the "normal" table""",
"""SHIFT, CTRL, ALT, SEARCH = 1, 2, 3, 4          # modifier markers in the "normal" table
# Special keys (v1.6, for the browser): codes above 127. The shell ignores them; programs read them from kb_getc().
KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT = 0x81, 0x82, 0x83, 0x84
KEY_F1 = 0x90                                   # F1..F10 = 0x90..0x99 (the top row; on a ChromeOS keyboard: back, forward, reload, ...)"""),
("""            'LEFTMETA': (chr(SEARCH),) * 2}.get(name, ('\\0', '\\0'))""",
"""            'LEFTMETA': (chr(SEARCH),) * 2,
            'UP': (chr(KEY_UP),) * 2, 'DOWN': (chr(KEY_DOWN),) * 2, 'LEFT': (chr(KEY_LEFT),) * 2, 'RIGHT': (chr(KEY_RIGHT),) * 2,
            **{'F%d' % n: (chr(KEY_F1 + n - 1),) * 2 for n in range(1, 11)}}.get(name, ('\\0', '\\0'))"""),
("""            ch, str(ord(ch)) if ord(ch) < 32 else "'%s'" % ch)""",
"""            ch, str(ord(ch)) if (ord(ch) < 32 or ord(ch) > 126) else "'%s'" % ch)"""),
("""           '#define KB_SHIFT %d' % SHIFT, '#define KB_CTRL %d' % CTRL, '#define KB_ALT %d' % ALT, '#define KB_SEARCH %d' % SEARCH]""",
"""           '#define KB_SHIFT %d' % SHIFT, '#define KB_CTRL %d' % CTRL, '#define KB_ALT %d' % ALT, '#define KB_SEARCH %d' % SEARCH,
           '#define K_UP %d' % KEY_UP, '#define K_DOWN %d' % KEY_DOWN, '#define K_LEFT %d' % KEY_LEFT, '#define K_RIGHT %d' % KEY_RIGHT,
           '#define K_F1 %d   /* F1..F10 = K_F1 .. K_F1+9 */' % KEY_F1]"""),
])
print('ok')
