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

def find_casts(text):
    """Find all C-style casts in text. Returns list of (start, end, type, expr)."""
    results = []
    i = 0
    while i < len(text):
        # Find '(' that starts a cast
        paren_idx = text.find('(', i)
        if paren_idx == -1:
            break

        # Check if this is a cast: ( type ) expr
        # type = identifier chars, *, spaces
        # Must be preceded by non-identifier (not part of a function call)
        # Must NOT be preceded by '[]' (lambda) or preceded by 'if'/'while'/etc

        # Skip if preceded by '[' (lambda param list) or identifier (function call)
        prev_char = text[paren_idx - 1] if paren_idx > 0 else '\n'
        if prev_char in '[_a-zA-Z0-9' or prev_char == ']':
            i = paren_idx + 1
            continue

        # Try to match cast type inside parens
        # Pattern: ( type-name * )
        m = re.match(r'\(\s*((?:const\s+)?(?:struct\s+)?\w+\s*\*?\s*)\)', text[paren_idx:])
        if not m:
            i = paren_idx + 1
            continue

        cast_type = m.group(1).strip()
        # Skip 'void' casts that are followed by a semicolon or newline — (void)expr; suppression
        after_cast = paren_idx + m.end()
        # Skip whitespace after )
        j = after_cast
        while j < len(text) and text[j] in ' \t':
            j += 1

        # Check if it's (void)arg; pattern (suppression)
        if cast_type == 'void':
            # Look for identifier followed by ; or )
            k = j
            while k < len(text) and (text[k].isalnum() or text[k] == '_'):
                k += 1
            rest = text[j:k].strip()
            if rest and k < len(text) and text[k] in ';)\n,':
                # This is likely (void)arg; — skip
                i = paren_idx + 1
                continue

        # Find the expression being cast
        expr_start = j
        if expr_start >= len(text):
            i = paren_idx + 1
            continue

        # Determine expression end
        expr_end = expr_start
        first_char = text[expr_start]

        if first_char == '"':
            # String literal
            expr_end = expr_start + 1
            while expr_end < len(text):
                if text[expr_end] == '\\' and expr_end + 1 < len(text):
                    expr_end += 2
                    continue
                if text[expr_end] == '"':
                    expr_end += 1
                    break
                expr_end += 1
        elif first_char == '&':
            expr_end += 1
            while expr_end < len(text) and (text[expr_end].isalnum() or text[expr_end] in '_.->[]'):
                if text[expr_end] == '[':
                    depth = 0
                    while expr_end < len(text):
                        if text[expr_end] == '[':
                            depth += 1
                        elif text[expr_end] == ']':
                            depth -= 1
                            if depth == 0:
                                expr_end += 1
                                break
                        expr_end += 1
                else:
                    expr_end += 1
        elif first_char.isalnum() or first_char == '_':
            # Identifier or function call
            while expr_end < len(text) and (text[expr_end].isalnum() or text[expr_end] == '_'):
                expr_end += 1
            # Handle function call: func(args)
            while expr_end < len(text) and text[expr_end] in '([':
                close = ')' if text[expr_end] == '(' else ']'
                depth = 0
                while expr_end < len(text):
                    if text[expr_end] in '([':
                        depth += 1
                    elif text[expr_end] in ')]':
                        depth -= 1
                        if depth == 0:
                            expr_end += 1
                            break
                    expr_end += 1
            # Handle .field or ->field
            while expr_end < len(text) and (text[expr_end] == '.' or
                                            text[expr_end:expr_end+2] == '->'):
                if text[expr_end] == '.':
                    expr_end += 1
                else:
                    expr_end += 2
                while expr_end < len(text) and (text[expr_end].isalnum() or text[expr_end] == '_'):
                    expr_end += 1
        else:
            # Not a recognizable expression
            i = paren_idx + 1
            continue

        expr = text[expr_start:expr_end].strip()
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
