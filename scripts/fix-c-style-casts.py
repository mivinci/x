#!/usr/bin/env python3
"""Fix C-style casts in .cpp files using regex replacement.

Finds C-style casts of the form (type)expr and replaces with the
appropriate C++ cast operator. Does not rely on clang-tidy (which
has issues with WarningsAsErrors + compilation errors).

Strategy:
  1. Find all (type)expr patterns in .cpp files
  2. Classify: pointer type → reinterpret_cast, numeric → static_cast
  3. Replace in-place

Excludes:
  - (void)identifier; — suppressed unused variable warning
  - Lambda parameter lists: [](void *) { ... }
  - Function parameter lists: foo(int x, (void *)y) — not a cast
"""

import os, re, sys

# Types that appear in C-style casts
CAST_TYPES = r'(?:const\s+)?(?:struct\s+)?(?:uint\d+_t|int\d+_t|size_t|ssize_t|int|unsigned|char|void|bool|float|double|off_t|long|short|signed|x[A-Z]\w+)'

def strip_comments(text):
    """Replace comment contents with spaces (preserving newlines) so
    that C-style casts inside comments are not mistaken for real casts.

    Skips over string/char literals (including C++ raw strings) so that
    comment markers inside strings (e.g. "http://x", "a /* b */ c") are
    not mistaken for real comment starts.

    The output is always the same length as the input — every character
    is either kept or replaced with a single space, never deleted — so
    index offsets returned by find_casts remain valid against the
    original text."""
    result = list(text)
    n = len(result)
    i = 0
    RAW_PREFIXES = ('R', 'LR', 'uR', 'UR', 'u8R')

    def collect_ident_prefix(idx):
        """Return the run of [A-Za-z0-9_] ending immediately before idx."""
        j = idx
        while j > 0 and (text[j - 1].isalnum() or text[j - 1] == '_'):
            j -= 1
        return text[j:idx]

    while i < n:
        c = result[i]

        # ── String literal ─────────────────────────────────────────
        if c == '"':
            prefix = collect_ident_prefix(i)
            if prefix in RAW_PREFIXES:
                # Raw string: R"delim(...)delim"
                delim_start = i + 1
                j = delim_start
                while j < n and text[j] != '(' and text[j] != '"':
                    j += 1
                if j >= n or text[j] != '(':
                    # Malformed (no '(' before next '"' or EOF) — fall
                    # back to normal-string handling so we at least skip
                    # the opening quote.
                    i = _skip_normal_string(text, i, n)
                    continue
                delim = text[delim_start:j]
                needle = ')' + delim + '"'
                end = text.find(needle, j + 1)
                if end == -1:
                    i = n  # unterminated; skip to EOF
                else:
                    i = end + len(needle)
                continue
            # Normal string (possibly L/u/U/u8 prefixed)
            i = _skip_normal_string(text, i, n)
            continue

        # ── Char literal — but skip C++14 digit separators (1'000) ──
        # A `'` preceded by a digit is treated as a separator and not
        # as the start of a char literal. This is a heuristic; it can
        # misclassify e.g. u8'a' (prev='8'), but the only consequence
        # is that we scan the char's contents as code — which is
        # harmless as long as those contents do not contain // or /*.
        if c == "'" and not (i > 0 and text[i - 1].isdigit()):
            j = i + 1
            while j < n:
                if text[j] == '\\' and j + 1 < n:
                    j += 2  # skip escape (incl. backslash-newline)
                    continue
                if text[j] == "'":
                    j += 1
                    break
                if text[j] == '\n':
                    j += 1  # unterminated; bail at newline
                    break
                j += 1
            i = j
            continue

        # ── Line comment: // ... \n ────────────────────────────────
        if c == '/' and i + 1 < n and result[i + 1] == '/':
            while i < n and result[i] != '\n':
                result[i] = ' '
                i += 1
            continue

        # ── Block comment: /* ... */ ────────────────────────────────
        if c == '/' and i + 1 < n and result[i + 1] == '*':
            result[i] = ' '
            result[i + 1] = ' '
            i += 2
            while i < n:
                if result[i] == '*' and i + 1 < n and result[i + 1] == '/':
                    result[i] = ' '
                    result[i + 1] = ' '
                    i += 2
                    break
                if result[i] != '\n':
                    result[i] = ' '
                i += 1
            continue

        i += 1

    return ''.join(result)


def _skip_normal_string(text, i, n):
    """Skip a normal (non-raw) string starting at text[i] == '"'.
    Handles \\" escapes and backslash-newline line continuations.
    Returns the index past the closing quote (or past the newline that
    ends an unterminated string, or past EOF)."""
    j = i + 1
    while j < n:
        if text[j] == '\\' and j + 1 < n:
            j += 2  # skip escape sequence (incl. backslash-newline)
            continue
        if text[j] == '"':
            return j + 1
        if text[j] == '\n':
            return j + 1  # unterminated; bail at newline
        j += 1
    return j

def find_casts(text):
    """Find all C-style casts in text. Returns list of (start, end, type, expr).

    Uses strip_comments() to avoid matching casts inside comments.
    Index offsets are valid for the original text.
    """
    stripped = strip_comments(text)
    results = []
    i = 0
    while i < len(stripped):
        # Find '(' that starts a cast
        paren_idx = stripped.find('(', i)
        if paren_idx == -1:
            break

        # Check if this is a cast: ( type ) expr
        # type = identifier chars, *, spaces
        # Must be preceded by non-identifier (not part of a function call)
        # Must NOT be preceded by '[]' (lambda) or preceded by 'if'/'while'/etc

        # Skip if preceded by '[' (lambda param list) or identifier (function call)
        prev_char = stripped[paren_idx - 1] if paren_idx > 0 else '\n'
        if prev_char in '[_a-zA-Z0-9' or prev_char == ']':
            i = paren_idx + 1
            continue

        # Try to match cast type inside parens
        # Pattern: ( type-name * )
        m = re.match(r'\(\s*((?:const\s+)?(?:struct\s+)?\w+\s*\*?\s*)\)', stripped[paren_idx:])
        if not m:
            i = paren_idx + 1
            continue

        cast_type = m.group(1).strip()
        # Skip 'void' casts that are followed by a semicolon or newline — (void)expr; suppression
        after_cast = paren_idx + m.end()
        # Skip whitespace after )
        j = after_cast
        while j < len(stripped) and stripped[j] in ' \t':
            j += 1

        # Check if it's (void)arg; pattern (suppression)
        if cast_type == 'void':
            # Look for identifier followed by ; or )
            k = j
            while k < len(stripped) and (stripped[k].isalnum() or stripped[k] == '_'):
                k += 1
            rest = stripped[j:k].strip()
            if rest and k < len(stripped) and stripped[k] in ';)\n,':
                # This is likely (void)arg; — skip
                i = paren_idx + 1
                continue

        # Find the expression being cast
        expr_start = j
        if expr_start >= len(stripped):
            i = paren_idx + 1
            continue

        # Determine expression end
        expr_end = expr_start
        first_char = stripped[expr_start]

        if first_char == '"':
            # String literal
            expr_end = expr_start + 1
            while expr_end < len(stripped):
                if stripped[expr_end] == '\\' and expr_end + 1 < len(stripped):
                    expr_end += 2
                    continue
                if stripped[expr_end] == '"':
                    expr_end += 1
                    break
                expr_end += 1
        elif first_char == '&':
            expr_end += 1
            while expr_end < len(stripped) and (stripped[expr_end].isalnum() or stripped[expr_end] in '_.->[]'):
                if stripped[expr_end] == '[':
                    depth = 0
                    while expr_end < len(stripped):
                        if stripped[expr_end] == '[':
                            depth += 1
                        elif stripped[expr_end] == ']':
                            depth -= 1
                            if depth == 0:
                                expr_end += 1
                                break
                        expr_end += 1
                else:
                    expr_end += 1
        elif first_char.isalnum() or first_char == '_':
            # Identifier or function call
            while expr_end < len(stripped) and (stripped[expr_end].isalnum() or stripped[expr_end] == '_'):
                expr_end += 1
            # Handle function call: func(args)
            while expr_end < len(stripped) and stripped[expr_end] in '([':
                close = ')' if stripped[expr_end] == '(' else ']'
                depth = 0
                while expr_end < len(stripped):
                    if stripped[expr_end] in '([':
                        depth += 1
                    elif stripped[expr_end] in ')]':
                        depth -= 1
                        if depth == 0:
                            expr_end += 1
                            break
                    expr_end += 1
            # Handle .field or ->field
            while expr_end < len(stripped) and (stripped[expr_end] == '.' or
                                            stripped[expr_end:expr_end+2] == '->'):
                if stripped[expr_end] == '.':
                    expr_end += 1
                else:
                    expr_end += 2
                while expr_end < len(stripped) and (stripped[expr_end].isalnum() or stripped[expr_end] == '_'):
                    expr_end += 1
        else:
            # Not a recognizable expression
            i = paren_idx + 1
            continue

        expr = stripped[expr_start:expr_end].strip()
        if not expr:
            i = paren_idx + 1
            continue

        # Verify cast_type is a known type (not just any identifier)
        type_base = cast_type.replace('const ', '').replace('struct ', '').replace('*', '').strip()
        if not re.match(r'^(uint\d+_t|int\d+_t|size_t|ssize_t|int|unsigned|char|void|bool|float|double|off_t|long|short|signed|x[A-Z]\w+)$', type_base):
            i = paren_idx + 1
            continue

        results.append((paren_idx, expr_end, cast_type, expr))
        i = expr_end

    return results

def choose_replacement(cast_type):
    """Choose the right C++ cast operator."""
    if '*' in cast_type:
        return 'reinterpret_cast'
    if cast_type == 'void':
        return None  # shouldn't reach here, handled above
    return 'static_cast'

def fix_file(filepath, dry_run=False):
    """Fix all C-style casts in a single file. Returns count of fixes."""
    with open(filepath) as f:
        content = f.read()

    casts = find_casts(content)
    if not casts:
        return 0

    # Apply fixes in reverse order to preserve indices
    new_content = content
    for start, end, cast_type, expr in reversed(casts):
        replacement_type = choose_replacement(cast_type)
        if replacement_type is None:
            replacement = expr  # just remove the cast
        else:
            replacement = f'{replacement_type}<{cast_type}>({expr})'
        new_content = new_content[:start] + replacement + new_content[end:]

    if new_content != content:
        if not dry_run:
            with open(filepath, 'w') as f:
                f.write(new_content)
        return len(casts)
    return 0

def main():
    dry_run = "--dry-run" in sys.argv

    roots = ["libx/x", "libdlproxy"]
    total = 0
    files_fixed = 0

    for root in roots:
        for r, _, files in os.walk(root):
            for f in sorted(files):
                if not f.endswith('.cpp'):
                    continue
                fpath = os.path.join(r, f)
                n = fix_file(fpath, dry_run=dry_run)
                if n > 0:
                    files_fixed += 1
                    total += n
                    action = "would fix" if dry_run else "fixed"
                    print(f"  {action} {n} cast(s) in {os.path.relpath(fpath)}")

    action = "Would fix" if dry_run else "Fixed"
    print(f"\n{action} {total} C-style cast(s) across {files_fixed} file(s)")

if __name__ == "__main__":
    main()
