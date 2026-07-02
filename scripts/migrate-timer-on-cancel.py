#!/usr/bin/env python3
"""
migrate-timer-on-cancel.py — Mechanical migration of xTimerStart call sites.

Transforms:
    xTimerStart(fn, arg, timeout, repeat)
into:
    xTimerStart(fn, arg, NULL, timeout, repeat)

Handles multi-line calls, nested parentheses, and C++ lambda arguments.
Skips the declaration in event.h and the definition in event_timer.c.
"""
import os
import sys


SKIP_FILES = {
    "libx/x/base/event.h",
    "libx/x/base/event_timer.c",
}


def split_top_level_commas(s):
    """Split a string on top-level commas, respecting (), [], {}."""
    parts = []
    depth = 0
    cur = []
    for ch in s:
        if ch in '([{':
            depth += 1
            cur.append(ch)
        elif ch in ')]}':
            depth -= 1
            cur.append(ch)
        elif ch == ',' and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append(''.join(cur))
    return parts


def transform_args(args_str):
    """Given args string (inside parens), if exactly 4 top-level args,
    insert NULL as third. Return transformed string or None if no change."""
    parts = split_top_level_commas(args_str)
    if len(parts) != 4:
        return None
    return parts[0] + "," + parts[1] + ", NULL," + parts[2] + "," + parts[3]


def find_call_range(s, start_idx):
    """Find next xTimerStart(...) call in s starting at start_idx.
    Returns (call_start, call_end, args_str) or None."""
    needle = "xTimerStart"
    i = start_idx
    n = len(s)
    while i < n:
        j = s.find(needle, i)
        if j == -1:
            return None
        # Word boundary check
        if j > 0 and (s[j - 1].isalnum() or s[j - 1] == '_'):
            i = j + 1
            continue
        k = j + len(needle)
        while k < n and s[k] in ' \t\r\n':
            k += 1
        if k >= n or s[k] != '(':
            i = j + 1
            continue
        depth = 1
        m = k + 1
        while m < n and depth > 0:
            ch = s[m]
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
                if depth == 0:
                    break
            m += 1
        if depth != 0:
            i = j + 1
            continue
        return (j, m + 1, s[k + 1:m])
    return None


def migrate_content(content):
    """Migrate all xTimerStart calls in content, including nested ones.
    Returns (new_content, count)."""
    # Process from end to start so earlier indices stay valid.
    # For nested calls, we process inner-first by scanning inside outer's args.
    # Simpler: scan content; for each top-level call found, recursively
    # transform its args (which handles nested calls), then replace the
    # outer call. Process top-level calls from end to start.
    top_calls = []
    i = 0
    while True:
        r = find_call_range(content, i)
        if r is None:
            break
        start, end, args = r
        # Recursively transform nested calls inside args
        new_args, inner_count = migrate_content(args)
        # Transform this call's args (after inner migration)
        outer_new = transform_args(new_args)
        if outer_new is None:
            outer_new = new_args
        top_calls.append((start, end, outer_new))
        i = end
    # Apply in reverse to preserve indices
    new_content = content
    for start, end, new_args in reversed(top_calls):
        new_content = new_content[:start] + "xTimerStart(" + new_args + ")" + new_content[end:]
    return new_content, len(top_calls)


def main():
    args = sys.argv[1:]
    dry_run = False
    if "--dry-run" in args:
        dry_run = True
        args.remove("--dry-run")
    paths = args or ["libx", "libdlproxy", "libxpp"]
    total = 0
    for root in paths:
        if os.path.isfile(root):
            n = migrate_file(root, dry_run)
            if n:
                print(f"{root}: {n} calls")
                total += n
        else:
            for dirpath, _, files in os.walk(root):
                for f in files:
                    if not f.endswith((".c", ".h", ".cpp", ".md")):
                        continue
                    p = os.path.join(dirpath, f)
                    n = migrate_file(p, dry_run)
                    if n:
                        print(f"{p}: {n} calls")
                        total += n
    print(f"Total: {total} call sites {'(dry-run)' if dry_run else ''}")
    return 0


def migrate_file(path, dry_run=False):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    rel = os.path.relpath(path)
    if rel in SKIP_FILES:
        return 0
    if "xTimerStart(" not in content:
        return 0
    new_content, n = migrate_content(content)
    if n == 0:
        return 0
    if not dry_run and new_content != content:
        with open(path, "w", encoding="utf-8") as f:
            f.write(new_content)
    return n


if __name__ == "__main__":
    sys.exit(main())
