"""Says where a file of the project has a sign that nobody can see: one that draws nothing, turns
a line around, ends a line as no key does, is a blank of another kind than the blank, or has no
meaning at all. Such a sign belongs in no file as itself. A test that needs one makes it from
its number where it runs; a text that needs one does not need it.

In the sources of the program and its tests, a letter is one of the twenty-six, small or large:
a letter of another alphabet, or one with a mark on it, may look like a letter it is not, which
is what the tests about that are for. (When the program's words are translated, the files of a
translation get a rule of their own: they are not sources, and some languages need signs that
are refused here.)

    python tools/check_signs.py [file ...]

Without files: every file git knows of under this folder, and every new one it does not ignore.
A file that is to be text and is not written in UTF-8 is said too: it cannot be looked through.
Ends with 1 where something was found.
"""
import pathlib
import subprocess
import sys
import unicodedata

HERE = pathlib.Path(__file__).resolve().parent.parent
TEXT = {
    '.cpp', '.h', '.hlsl', '.hlsli', '.qml', '.js', '.md', '.py', '.ps1', '.cmake', '.txt', '.json', '.rc', '.in',
    '.qrc', '.svg', '.xml', '.manifest', '.toml', '.yml', '.yaml', '.ini', '.cfg', '.bat', '.sh', '.html', '.css',
    '.clang-format', '.clang-tidy', '.gitignore', '.gitattributes',
}
SOURCES = {'.cpp', '.h', '.hlsl', '.hlsli', '.qml', '.js'}
# Never as themselves: format signs (zero width, the ones that turn a line around, soft hyphens,
# tags), controls other than a tab and a line's end, the two separators that end a line without
# being a line's end, blanks other than the blank, signs for private use and signs that are none.
UNSEEN = {'Cf', 'Cc', 'Zl', 'Zp', 'Zs', 'Co', 'Cn', 'Cs'}
# And those that are not of a kind that says so of itself: letters that are blanks, and the marks
# that pick one drawing of the letter before them or join it to the next. (The list the server
# keeps, and the program's own for what a file says: ui/files_view.cpp.)
NOT_DRAWN = (
    {0x034F, 0x115F, 0x1160, 0x17B4, 0x17B5, 0x2800, 0x3164, 0xFFA0}
    | set(range(0x180B, 0x1810))
    | set(range(0xFE00, 0xFE10))
    | set(range(0xE0100, 0xE01F0))
)


def files():
    if len(sys.argv) > 1:
        return [pathlib.Path(name) for name in sys.argv[1:]]
    # (With -z git gives the names as they are, and not in quotes where one has an odd sign in it.)
    listed = subprocess.run(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', '.'], cwd=HERE,
                            capture_output=True, check=True).stdout.decode('utf-8', 'surrogateescape').split('\0')
    return [HERE / name for name in listed if name]


def is_text(path):
    return path.suffix.lower() in TEXT or path.name.lower() in TEXT or path.name in ('CMakeLists.txt', 'LICENSE')


def wrong(character, after, in_sources):
    number = ord(character)
    if number == 0x20 or character in '\t\n':
        return False
    # (A line's end may be written with two signs, as Windows writes it; the first alone is none.)
    if character == '\r':
        return after != '\n'
    if number < 0x20 or number == 0x7F:
        return True
    if number < 0x7F:
        return False
    if unicodedata.category(character) in UNSEEN or number in NOT_DRAWN:
        return True
    # (In sources: no letter but the twenty-six, and no mark that sits on one and makes another of it.)
    return in_sources and unicodedata.category(character)[0] in 'LM'


found = 0
looked = 0
for path in files():
    if not is_text(path) or not path.is_file():
        continue
    looked += 1
    try:
        text = path.read_bytes().decode('utf-8')
    except UnicodeDecodeError:
        found += 1
        print(f'{path}: not written in UTF-8, and so not looked through')
        continue
    # (The mark some programs put at a file's head is no part of what it says.)
    if text.startswith(chr(0xFEFF)):
        text = text[1:]
    in_sources = path.suffix.lower() in SOURCES
    line = 1
    for at, character in enumerate(text):
        if wrong(character, text[at + 1:at + 2], in_sources):
            found += 1
            print(f'{path}:{line}: U+{ord(character):04X} {unicodedata.name(character, "a sign without a name")}')
        if character == '\n':
            line += 1
print(f'{looked} files looked through.')
if found:
    print(f'{found} signs that nobody can see, or that look like others.')
sys.exit(1 if found else 0)
