"""Tries check_signs.py on files made for it, each with one sign in it that it is to refuse or
to let pass. A check that refuses nothing would pass every time it is run.

    python tools/test_check_signs.py

Every sign is made from its number here, too. Ends with 1 where the check did other than it should.
"""
import pathlib
import subprocess
import sys
import tempfile

CHECK = pathlib.Path(__file__).with_name('check_signs.py')
# What it is, the file's name (its ending decides what is looked for), what is in it, and whether
# the check is to refuse it.
CASES = [
    ('a blank of no width in a text', 'a.md', 'one' + chr(0x200B) + 'two', True),
    ('the sign that turns a line around, in a source', 'a.cpp', '// x' + chr(0x202E) + 'y', True),
    ('a blank that does not break', 'b.md', 'one' + chr(0x00A0) + 'two', True),
    ('a letter that is a blank', 'c.md', 'one' + chr(0x3164) + 'two', True),
    ('a mark that picks a drawing', 'd.md', 'one' + chr(0xFE0F) + 'two', True),
    ('a letter of another alphabet in a source', 'b.cpp', 'auto ' + chr(0x0430) + ' = 1;', True),
    ('a letter with a mark on it in a source', 'c.cpp', '// caf' + chr(0x00E9), True),
    ('a letter and a mark that sits on it, in a source', 'e.cpp', '// cafe' + chr(0x0301), True),
    ('a letter and a mark that sits on it, in a text', 'l.md', 'cafe' + chr(0x0301), False),
    ('a letter of another alphabet in a header', 'a.h', '// ' + chr(0x0430), True),
    ('a letter of another alphabet in a shader', 'b.hlsl', '// ' + chr(0x0430), True),
    ('a letter of another alphabet in what a shader takes in', 'a.hlsli', '// ' + chr(0x0430), True),
    ('a letter of another alphabet in a screen', 'b.qml', '// ' + chr(0x0430), True),
    ('a letter of another alphabet in a script of a screen', 'a.js', '// ' + chr(0x0430), True),
    ('a letter of another alphabet in a text', 'e.md', 'see ' + chr(0x0430), False),
    ('a line that ends with the first of two signs alone', 'f.md', 'one\rtwo', True),
    ('lines that end as Windows writes them', 'g.md', 'one\r\ntwo\r\n', False),
    ('the sign for taking a letter away', 'h.md', 'one' + chr(0x7F), True),
    ('a dot between and an arrow in a source', 'd.cpp', '// a ' + chr(0x00B7) + ' b ' + chr(0x21B5), False),
    ('a blank of no width in a shader', 'a.hlsl', '// x' + chr(0x200B), True),
    ('a blank of no width in a screen', 'a.qml', '// x' + chr(0x200B), True),
    ('nothing odd', 'i.md', 'nothing odd\n\tat all\n', False),
    ('the mark at a file\'s head', 'k.md', chr(0xFEFF) + 'said after it\n', False),
    # Dashes other than the hyphen, the minus sign and quotes that curl: written nowhere (the owner, 2026-11-13).
    ('a long dash in a text', 'm.md', 'one ' + chr(0x2014) + ' two', True),
    ('a long dash in a source', 'f.cpp', 'auto a = u" ' + chr(0x2014) + ' "_s;', True),
    ('a short dash between numbers in a screen', 'c.qml', 'text: "1' + chr(0x2013) + '2"', True),
    ('a dash that does not break', 'n.md', 'one' + chr(0x2011) + 'two', True),
    ('the minus sign', 'o.md', 'a ' + chr(0x2212) + ' b', True),
    ('a quote that curls, in a source', 'g.cpp', '// it' + chr(0x2019) + 's', True),
    ('quotes that curl, in a screen', 'd.qml', 'text: "' + chr(0x201C) + 'a' + chr(0x201D) + '"', True),
    ('quotes that point, in a text', 'p.md', chr(0x00AB) + 'a' + chr(0x00BB), True),
    ('the hyphen and straight quotes', 'q.md', 'well-known - "a" and \'b\'', False),
    ('three dots made one, in a text', 'r.md', 'Sending' + chr(0x2026), False),
    ('three dots made one, in a source', 'h.cpp', 'auto a = u"Sending' + chr(0x2026) + '"_s;', False),
    ('a low quote that curls', 's.md', chr(0x201E) + 'a' + chr(0x201C), True),
    ('a prime', 't.md', '5' + chr(0x2032), True),
    ('an accent alone', 'u.md', 'it' + chr(0x00B4) + 's', True),
    ('a line of a box', 'v.md', 'one ' + chr(0x2500) + ' two', True),
    ('a quote of full width', 'w.md', chr(0xFF02) + 'a' + chr(0xFF02), True),
    ('a long dash in a setup script', 'a.iss', 'Name: "one ' + chr(0x2014) + ' two"', True),
    ('a long dash made from its number, in a source', 'j.cpp', 'said += QChar(0x2014);', True),
    ('a quote that curls made from its number, in a source', 'k.cpp', 'said += QChar( 0x201C );', True),
    ('a long dash made from its number, in a screen', 'f.qml', 'text: String.fromCharCode(0x2014)', True),
    ('a long dash written as an escape, in a screen', 'g.qml', 'text: "a \\u2014 b"', True),
    ('the number of a quote compared, in a source', 'l.cpp', 'return sign >= 0x201C && sign <= 0x201F;', False),
    ('a long dash made from its number, in a test', 'tests/m.cpp', 'auto dash = QChar(0x2014);', False),
]


def refuses(path):
    return subprocess.run([sys.executable, str(CHECK), str(path)], capture_output=True, check=False).returncode == 1


wrong = 0
with tempfile.TemporaryDirectory() as folder:
    for what, name, text, is_refused in CASES:
        path = pathlib.Path(folder) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode('utf-8'))
        if refuses(path) != is_refused:
            wrong += 1
            print(f'{what}: {"let pass" if is_refused else "refused"}, and should have been {"refused" if is_refused else "let pass"}')
    # A file that is to be text and is written otherwise cannot be looked through: said.
    path = pathlib.Path(folder) / 'j.md'
    path.write_bytes(b'caf\xe9')
    if not refuses(path):
        wrong += 1
        print('a file that is not UTF-8: let pass, and should have been refused')
print(f'{len(CASES) + 1} tried, {wrong} wrong')
sys.exit(1 if wrong else 0)
