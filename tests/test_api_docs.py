"""docs/api.md covers the public interface: every public name declared in include/enaga/ appears in it.

Host-only (no emulator, no compiler). The names are gathered from the headers with regular expressions, which
is enough for their style: C names (ENAGA_*, enaga_*, the host table's functions) from the .h files; from the
.hpp files, classes, structs, enums and their values, functions and member functions, constants and type
aliases, leaving out `namespace detail` and `private:` sections. A name counts as documented when it appears in
api.md as a whole word.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "include" / "enaga"
API = ROOT / "docs" / "api.md"

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "static_assert", "case", "do", "else", "operator",
            "new", "delete", "alignof", "decltype", "noexcept"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def drop_block(text, opening):
    """Removes every `opening ... {` block, braces matched, e.g. namespace detail { ... }."""
    while True:
        m = re.search(opening, text)
        if not m:
            return text
        depth, i = 0, text.index("{", m.start())
        for j in range(i, len(text)):
            depth += {"{": 1, "}": -1}.get(text[j], 0)
            if depth == 0:
                break
        text = text[:m.start()] + text[j + 1:]


def drop_private(text):
    """Removes each `private:` section up to the end of its class (the brace that closes the class)."""
    while True:
        m = re.search(r"\bprivate:", text)
        if not m:
            return text
        depth = 0
        for j in range(m.end(), len(text)):
            depth += {"{": 1, "}": -1}.get(text[j], 0)
            if depth < 0:
                break
        text = text[:m.start()] + text[j:]


def c_names(text):
    code = strip_comments(text)
    names = set(re.findall(r"\b(?:ENAGA|enaga)_\w+\b", code))
    names |= set(re.findall(r"\(\*(\w+)\)\s*\(", code))                 # host and app table functions
    return {n for n in names if not n.endswith("_H")}                   # not the include guards


def cpp_names(text):
    code = drop_private(drop_block(strip_comments(text), r"\bnamespace\s+detail\s*\{"))
    code = re.sub(r"#define[^\n]*(?:\\\n[^\n]*)*", " ", code)
    names = set(re.findall(r"\b(?:class|struct)\s+(\w+)\s*(?::|\{)", code))
    for enum in re.finditer(r"\benum\s+(?:class\s+)?(\w+)?[^{;]*\{([^}]*)\}", code):
        if enum.group(1):
            names.add(enum.group(1))
        names |= set(re.findall(r"(\w+)\s*(?:=[^,]*)?(?:,|$)", enum.group(2).strip()))
    names |= {m for m in re.findall(r"\b(\w+)\s*\([^;{}]*\)\s*(?:const\s*)?(?:override\s*)?(?:noexcept\s*)?\{", code)}
    for declaration in re.findall(r"\bconstexpr\s+[\w:]+\s+([^;(]*);", code):    # int a = 1, b = 2;
        names |= set(re.findall(r"(\w+)\s*=", declaration))
    names |= set(re.findall(r"\busing\s+(\w+)\s*=", code))
    # Declarations at namespace level (fatal, host): a return type, the name, the parameters, a semicolon.
    names |= set(re.findall(r"^(?:\[\[\w+\]\]\s*)?(?:const\s+)?(?!return\b)[\w:]+\s*[&*]?\s*(\w+)\s*\([^;{}=]*\)\s*;",
                            code, flags=re.M))
    return {n for n in names if n not in KEYWORDS and not n.startswith("ENAGA_") and n != "Type"}


def public_names():
    names = {}
    for path in sorted(INCLUDE.rglob("*")):
        if path.suffix == ".h":
            found = c_names(path.read_text())
        elif path.suffix == ".hpp":
            text = path.read_text()
            found = cpp_names(text) | c_names(text)
        else:
            continue
        for name in found:
            names.setdefault(name, path.relative_to(ROOT))
    return names


class ApiDocs(unittest.TestCase):
    def test_every_public_name_is_in_api_md(self):
        doc = API.read_text()
        words = set(re.findall(r"\w+", doc))
        missing = sorted(f"{name} ({path})" for name, path in public_names().items() if name not in words)
        self.assertEqual(missing, [], "names in include/enaga/ not mentioned in docs/api.md")

    def test_the_extraction_finds_names(self):
        # Guards the check itself: a regex that stopped matching would pass every header.
        names = public_names()
        for expected in ("ENAGA_KEY_F1", "enaga_svf_process", "file_save", "App", "on_pad_press", "Stream",
                         "open_menu", "SvfCoefficients", "gate_on", "sample_hold", "frame_bytes", "max_keys"):
            self.assertIn(expected, names)
        self.assertNotIn("u16", names)          # wav.hpp's namespace detail
        self.assertNotIn("make_path", names)    # Settings' private part
        self.assertGreater(len(names), 300)


if __name__ == "__main__":
    unittest.main()
