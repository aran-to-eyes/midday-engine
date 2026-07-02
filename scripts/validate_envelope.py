#!/usr/bin/env python3
"""validate_envelope.py — validate CLI output against formats/cli_envelope.schema.json.

Reads one JSON document from stdin and validates it against the envelope
schema. Dependency-free by design (the pinned tool set is cmake, ninja,
python3, git, jq, zstd — no pip): implements exactly the JSON Schema 2020-12
keyword subset the envelope schema uses, and REFUSES schemas that use
keywords outside that subset, so the schema cannot silently outgrow the
validator.

Usage:
    build/dev/midday version --json | scripts/validate_envelope.py [schema.json]
"""

from __future__ import annotations

import json
import pathlib
import sys

HANDLED = {
    "$schema", "$id", "title", "description",  # annotations
    "type", "required", "properties", "additionalProperties",
    "enum", "const", "minLength", "allOf", "if", "then", "not",
}

TYPES = {
    "object": dict,
    "array": list,
    "string": str,
    "boolean": bool,
    "null": type(None),
}


def check_type(value, expected: str) -> bool:
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, TYPES[expected])


def validate(value, schema: dict, path: str, errors: list[str]) -> bool:
    """Appends to errors; returns True iff value satisfies schema."""
    unknown = set(schema) - HANDLED
    if unknown:
        raise SystemExit(
            f"schema uses keywords outside the validator subset: {sorted(unknown)} "
            f"at {path or '$'} — extend scripts/validate_envelope.py in the same commit."
        )

    before = len(errors)

    if "type" in schema and not check_type(value, schema["type"]):
        errors.append(f"{path or '$'}: expected type {schema['type']}, got {type(value).__name__}")
        return False
    if "const" in schema and value != schema["const"]:
        errors.append(f"{path or '$'}: expected const {schema['const']!r}, got {value!r}")
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path or '$'}: {value!r} not in enum {schema['enum']}")
    if "minLength" in schema and isinstance(value, str) and len(value) < schema["minLength"]:
        errors.append(f"{path or '$'}: string shorter than minLength {schema['minLength']}")

    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{path or '$'}: missing required property {key!r}")
        for key, subschema in schema.get("properties", {}).items():
            if key in value:
                validate(value[key], subschema, f"{path}.{key}", errors)
        if schema.get("additionalProperties") is False:
            allowed = set(schema.get("properties", {}))
            for key in value:
                if key not in allowed:
                    errors.append(f"{path or '$'}: additional property {key!r} not allowed")

    if "not" in schema:
        scratch: list[str] = []
        if validate(value, schema["not"], path, scratch):
            errors.append(f"{path or '$'}: matches forbidden 'not' schema {schema['not']}")

    for i, subschema in enumerate(schema.get("allOf", [])):
        cond = subschema.get("if")
        if cond is not None:
            scratch = []
            if validate(value, cond, path, scratch) and "then" in subschema:
                validate(value, subschema["then"], f"{path}(allOf[{i}].then)", errors)
        else:
            validate(value, subschema, f"{path}(allOf[{i}])", errors)

    return len(errors) == before


def main() -> int:
    repo = pathlib.Path(__file__).resolve().parent.parent
    schema_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else (
        repo / "formats" / "cli_envelope.schema.json"
    )
    schema = json.loads(schema_path.read_text())
    try:
        document = json.load(sys.stdin)
    except json.JSONDecodeError as exc:
        print(json.dumps({"ok": False, "errors": [f"stdin is not JSON: {exc}"]}))
        return 1

    errors: list[str] = []
    validate(document, schema, "", errors)
    print(json.dumps({"ok": not errors, "schema": schema_path.name, "errors": errors}, indent=2))
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
