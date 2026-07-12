#!/usr/bin/env python3
r"""check_concerns.py — the concerns ledger becomes an ENFORCED gate (M1-exit
Phase 4, Hackathon #1 anti-recurrence #2: "prose-to-enforcement", the same
lesson CONCERNS #6 taught for logging purity).

The ledger (.claude/build/CONCERNS.md) is deliberately untracked (no agent
traces in git), so this gate BITES where the ledger lives — the dev machine
running scripts/verify.sh. On a checkout without the file (CI verify-linux,
fresh clones) it self-skips with a printed note: enforcing an absent file
would be a vacuous pass pretending to be coverage, and committing the ledger
is not on the table. If the ledger ever becomes TRACKED, that is itself an
error (the no-agent-traces invariant broke silently).

Rules (each falsified by --self-test, one plant per sub-rule, before any
real run is trusted — the council's G6: a multi-violation plant lets
individual rules regress green):
  1. GRAMMAR   — every `## N. [sev] title — \`status\`` header parses; the
                 status is comma-split into clauses and EVERY clause must
                 match `[sub-id] token [(note)]` with token in done | open |
                 parked | next-up | deferred (anchored — `not done` is a
                 parse error, not a done). Duplicate item numbers error.
                 A ledger with zero parseable items errors (truncation must
                 never read as clean).
  2. DEFERRAL  — any item not entirely `done` must carry, in its body: a
                 CALENDAR-VALID date (YYYY-MM-DD), a named owner (m<k>/M<k>
                 milestone, node id, D-BUILD ref, or Hackathon #k), and a
                 `guard:` naming a real mechanism (non-empty, not "none").
  3. BOUNDARY  — with --boundary (the milestone-exit ritual), any item the
                 ledger classes fix-first (case-insensitive, in its body or
                 the file-level "**Fix-first:**" paragraph — full paragraph,
                 not just its first line) that is not `done` is an ERROR:
                 no fix-first item survives past one boundary. That is
                 exactly how CONCERNS #1 nearly rotted M0 -> M1.
"""

import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

LEDGER = Path(".claude/build/CONCERNS.md")
HEADER_RE = re.compile(r"^## (\d+)\. \[([^\]]+)\] (.+?) — `(.+)`\s*$")
CLAUSE_RE = re.compile(
    r"^(?:\d+[a-z]?\s+)?(done|open|parked|next-up|deferred)(?:\s*\([^)]*\))?$")
DATE_RE = re.compile(r"\b(20\d{2}-\d{2}-\d{2})\b")
OWNER_RE = re.compile(
    r"\b(m\d+[a-z0-9-]*|M\d+(?:-[a-z]+)?|D-BUILD-\d+|Hackathon #\d+)\b")
GUARD_RE = re.compile(r"\bguard:\s*(?!none\b)\S[^\n]{11,}", re.IGNORECASE)
FIXFIRST_PARA_RE = re.compile(r"^\*\*Fix-first:\*\*(.*?)(?:\n\s*\n|\Z)",
                              re.MULTILINE | re.DOTALL)
FIXFIRST_ITEM_RE = re.compile(r"#(\d+)\b")


def parse_status(status):
    """-> list of tokens, or None if any clause fails the anchored grammar."""
    tokens = []
    for clause in status.split(","):
        m = CLAUSE_RE.match(clause.strip())
        if not m:
            return None
        tokens.append(m.group(1))
    return tokens


def has_valid_date(body):
    for d in DATE_RE.findall(body):
        try:
            datetime.date.fromisoformat(d)
            return True
        except ValueError:
            continue
    return False


def parse_sections(text, errors):
    """-> sections {num: (status, body)}; duplicate numbers -> rule-1 error."""
    sections = {}
    current, body_lines = None, []

    def store():
        if current is None:
            return
        num = current[0]
        if num in sections:
            errors.append(f"duplicate item number #{num} (rule 1)")
        sections[num] = (current[1], "\n".join(body_lines))

    for line in text.splitlines():
        if line.startswith("## "):
            store()
            body_lines = []
            m = HEADER_RE.match(line)
            if m:
                current = (int(m.group(1)), m.group(4))
            elif re.match(r"^## \d+\.", line):
                errors.append(f"unparseable item header (rule 1): {line}")
                current = None
            else:
                current = None  # prose section ("## Genuinely solid ...")
        elif current is not None:
            body_lines.append(line)
    store()
    return sections


def check(text, boundary):
    errors = []
    sections = parse_sections(text, errors)
    if not sections:
        errors.append("no parseable concern items (rule 1) — truncated or "
                      "renamed ledger must never read as clean")
        return errors
    fixfirst = set()
    for para in FIXFIRST_PARA_RE.finditer(text):
        fixfirst.update(int(n) for n in FIXFIRST_ITEM_RE.findall(para.group(1)))
    for num, (status, body) in sorted(sections.items()):
        tokens = parse_status(status)
        if tokens is None:
            errors.append(f"#{num}: status `{status}` fails the clause "
                          "grammar (rule 1)")
            continue
        if re.search(r"fix-first", body, re.IGNORECASE):
            fixfirst.add(num)
        if all(t == "done" for t in tokens):
            continue
        missing = []
        if not has_valid_date(body):
            missing.append("calendar-valid date (YYYY-MM-DD)")
        if not OWNER_RE.search(body):
            missing.append("named owner (m<k>/M<k>/node/D-BUILD/Hackathon)")
        if not GUARD_RE.search(body):
            missing.append("`guard:` naming a real mechanism")
        if missing:
            errors.append(f"#{num}: open without {', '.join(missing)} (rule 2)")
        if boundary and num in fixfirst:
            errors.append(
                f"#{num}: fix-first and not done at a milestone boundary "
                "(rule 3) — pay it or re-class it explicitly before exit")
    return errors


def ledger_is_tracked():
    """The ledger entering git is itself a violation (no agent traces)."""
    try:
        r = subprocess.run(["git", "ls-files", "--error-unmatch", str(LEDGER)],
                           capture_output=True, timeout=10)
        return r.returncode == 0
    except Exception:
        return False  # no git available -> nothing to assert here


GOOD_ITEM = ("## 2. [low] deferred item — `open`\n"
             "Deferred 2026-07-12 -> m4-warden-assets. "
             "guard: audit reports it under out_of_scope every run.\n")
SELFTEST_CLEAN = "## 1. [high] paid item — `done`\nAll done.\n" + GOOD_ITEM
# One plant per sub-rule; `expect` must appear in some error, and the plant
# must be the ONLY violation so a single regressed sub-rule cannot hide.
SELFTEST_PLANTS = [
    ("unparseable header", "## 3. [low] broken header - no backtick status\nx\n",
     "unparseable item header"),
    ("unknown status token", "## 3. [low] alien — `wip`\n"
     "Deferred 2026-07-12 -> m2. guard: something real enough here.\n",
     "fails the clause grammar"),
    ("negated done", "## 3. [low] sneaky — `not done`\n"
     "Deferred 2026-07-12 -> m2. guard: something real enough here.\n",
     "fails the clause grammar"),
    ("missing date", "## 3. [low] undated — `open`\n"
     "-> m2 owns it. guard: something real enough here.\n",
     "calendar-valid date"),
    ("invalid date", "## 3. [low] fake-dated — `open`\n"
     "Deferred 2026-99-99 -> m2. guard: something real enough here.\n",
     "calendar-valid date"),
    ("missing owner", "## 3. [low] unowned — `open`\n"
     "Deferred 2026-07-12. guard: something real enough here.\n",
     "named owner"),
    ("missing guard", "## 3. [low] unguarded — `open`\n"
     "Deferred 2026-07-12 -> m2, no mechanism.\n",
     "guard"),
    ("empty guard", "## 3. [low] hollow — `open`\n"
     "Deferred 2026-07-12 -> m2. guard: none\n",
     "guard"),
    ("duplicate number", "## 2. [low] clone — `done`\nAll done.\n",
     "duplicate item number"),
    ("fix-first body at boundary", "## 3. [high] urgent — `open`\n"
     "Fix-First class. Deferred 2026-07-12 -> m2. "
     "guard: something real enough here.\n",
     "rule 3"),
    ("fix-first wrapped paragraph", "## 3. [high] urgent — `open`\n"
     "Deferred 2026-07-12 -> m2. guard: something real enough here.\n"
     "\n**Fix-first:** the worst items are\n#3 and nothing else.\n",
     "rule 3"),
]


def selftest():
    errs = check(SELFTEST_CLEAN, boundary=True)
    if errs:
        print(f"self-test FAILED: clean doc flagged: {errs}", file=sys.stderr)
        return 1
    if not any("no parseable" in e for e in check("just prose\n", boundary=False)):
        print("self-test FAILED: empty ledger not caught", file=sys.stderr)
        return 1
    for name, plant, expect in SELFTEST_PLANTS:
        errs = check(SELFTEST_CLEAN + plant, boundary=True)
        if not any(expect in e for e in errs):
            print(f"self-test FAILED: planted '{name}' not caught "
                  f"(expected '{expect}', got {errs})", file=sys.stderr)
            return 1
        # the plant must be the sole complaint source (position independence)
        if any("#2:" in e or "#1:" in e for e in errs):
            print(f"self-test FAILED: plant '{name}' bled into clean items: "
                  f"{errs}", file=sys.stderr)
            return 1
    print(f"check_concerns self-test: all {len(SELFTEST_PLANTS) + 1} planted "
          "violations caught, clean doc passes")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boundary", action="store_true",
                    help="milestone-exit mode: fix-first items must be done")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return selftest()
    if not LEDGER.exists():
        print(f"check_concerns: {LEDGER} absent (untracked ledger — CI/fresh "
              "checkout); nothing to enforce here, the dev-machine gate owns this")
        return 0
    if ledger_is_tracked():
        print(f"check_concerns: {LEDGER} is TRACKED in git — the "
              "no-agent-traces invariant broke; untrack it (git rm --cached) "
              "and fix .gitignore", file=sys.stderr)
        return 1
    errors = check(LEDGER.read_text(encoding="utf-8"), boundary=args.boundary)
    if errors:
        print("concerns ledger violations:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    print(f"check_concerns: ledger clean "
          f"({'boundary' if args.boundary else 'standing'} rules)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
