// ts/codegen/json.ts — the byte-exact JSON layer of the self-hosted
// generator. The parser is strict (RFC 8259 + core/base extras: duplicate
// keys rejected, depth capped, -0 stays a double) and keeps int64 values as
// their canonical source tokens, because JS numbers cannot hold them. The
// writer reproduces core/base/json_write.cpp byte-for-byte: compact,
// insertion-ordered, integers exact, doubles as dragonbox-shortest digits
// formatted with the to_chars(general) fixed/scientific rule. Byte contract:
// api/CODEGEN.md "Numbers".

export type JValue =
    | { k: "null" }
    | { k: "bool"; v: boolean }
    | { k: "int"; raw: string } // canonical int64 token, emitted verbatim
    | { k: "dbl"; v: number }
    | { k: "str"; v: string }
    | { k: "arr"; v: JValue[] }
    | JObject;

export interface JObject {
    k: "obj";
    v: [string, JValue][];
}

export class JsonParseError extends Error {
    constructor(
        message: string,
        readonly origin: string,
        readonly line: number,
        readonly col: number,
    ) {
        super(origin + ":" + line + ":" + col + ": " + message);
    }
}

const I64_MIN = -(2n ** 63n);
const I64_MAX = 2n ** 63n - 1n;
const MAX_DEPTH = 128; // core/base Json::kMaxParseDepth

// ---------------------------------------------------------------- parsing

class Parser {
    private pos = 0;

    constructor(
        private readonly text: string,
        private readonly origin: string,
    ) {}

    parseDocument(): JValue {
        const value = this.parseValue(0);
        this.skipWs();
        if (this.pos < this.text.length)
            this.fail("trailing characters after the JSON document");
        return value;
    }

    private fail(message: string): never {
        let line = 1;
        let lineStart = 0;
        for (let i = 0; i < this.pos; ++i)
            if (this.text[i] === "\n") {
                ++line;
                lineStart = i + 1;
            }
        throw new JsonParseError(message, this.origin, line, this.pos - lineStart + 1);
    }

    private skipWs(): void {
        while (this.pos < this.text.length) {
            const c = this.text[this.pos];
            if (c !== " " && c !== "\t" && c !== "\n" && c !== "\r") return;
            ++this.pos;
        }
    }

    private literal(word: string, value: JValue): JValue {
        if (this.text.startsWith(word, this.pos)) {
            this.pos += word.length;
            return value;
        }
        this.fail("invalid literal, expected '" + word + "'");
    }

    private parseValue(depth: number): JValue {
        this.skipWs();
        if (this.pos >= this.text.length) this.fail("unexpected end of input");
        const c = this.text[this.pos];
        if (c === "{") return this.parseObject(depth);
        if (c === "[") return this.parseArray(depth);
        if (c === '"') return { k: "str", v: this.parseString() };
        if (c === "t") return this.literal("true", { k: "bool", v: true });
        if (c === "f") return this.literal("false", { k: "bool", v: false });
        if (c === "n") return this.literal("null", { k: "null" });
        if (c === "-" || (c >= "0" && c <= "9")) return this.parseNumber();
        this.fail("unexpected character '" + c + "'");
    }

    private parseObject(depth: number): JValue {
        if (depth + 1 > MAX_DEPTH) this.fail("nesting depth exceeds the maximum of " + MAX_DEPTH);
        ++this.pos; // '{'
        const pairs: [string, JValue][] = [];
        this.skipWs();
        if (this.text[this.pos] === "}") {
            ++this.pos;
            return { k: "obj", v: pairs };
        }
        for (;;) {
            this.skipWs();
            if (this.text[this.pos] !== '"') this.fail("expected an object key string");
            const key = this.parseString();
            for (const [seen] of pairs)
                if (seen === key) this.fail("duplicate object key '" + key + "'");
            this.skipWs();
            if (this.text[this.pos] !== ":") this.fail("expected ':' after the object key");
            ++this.pos;
            pairs.push([key, this.parseValue(depth + 1)]);
            this.skipWs();
            const next = this.text[this.pos];
            if (next === ",") {
                ++this.pos;
                continue;
            }
            if (next === "}") {
                ++this.pos;
                return { k: "obj", v: pairs };
            }
            this.fail("expected ',' or '}' in object");
        }
    }

    private parseArray(depth: number): JValue {
        if (depth + 1 > MAX_DEPTH) this.fail("nesting depth exceeds the maximum of " + MAX_DEPTH);
        ++this.pos; // '['
        const items: JValue[] = [];
        this.skipWs();
        if (this.text[this.pos] === "]") {
            ++this.pos;
            return { k: "arr", v: items };
        }
        for (;;) {
            items.push(this.parseValue(depth + 1));
            this.skipWs();
            const next = this.text[this.pos];
            if (next === ",") {
                ++this.pos;
                continue;
            }
            if (next === "]") {
                ++this.pos;
                return { k: "arr", v: items };
            }
            this.fail("expected ',' or ']' in array");
        }
    }

    private parseHex4(): number {
        const hex = this.text.slice(this.pos, this.pos + 4);
        if (!/^[0-9a-fA-F]{4}$/.test(hex)) this.fail("invalid \\u escape");
        this.pos += 4;
        return parseInt(hex, 16);
    }

    private parseString(): string {
        ++this.pos; // '"'
        let out = "";
        for (;;) {
            if (this.pos >= this.text.length) this.fail("unterminated string");
            const c = this.text[this.pos];
            if (c === '"') {
                ++this.pos;
                return out;
            }
            if (c.charCodeAt(0) < 0x20) this.fail("unescaped control character in string");
            if (c !== "\\") {
                out += c;
                ++this.pos;
                continue;
            }
            ++this.pos;
            const escape = this.text[this.pos];
            ++this.pos;
            if (escape === '"' || escape === "\\" || escape === "/") out += escape;
            else if (escape === "b") out += "\b";
            else if (escape === "f") out += "\f";
            else if (escape === "n") out += "\n";
            else if (escape === "r") out += "\r";
            else if (escape === "t") out += "\t";
            else if (escape === "u") {
                const unit = this.parseHex4();
                if (unit >= 0xd800 && unit <= 0xdbff) {
                    if (this.text.slice(this.pos, this.pos + 2) !== "\\u")
                        this.fail("unpaired surrogate escape");
                    this.pos += 2;
                    const low = this.parseHex4();
                    if (low < 0xdc00 || low > 0xdfff) this.fail("unpaired surrogate escape");
                    out += String.fromCharCode(unit, low);
                } else if (unit >= 0xdc00 && unit <= 0xdfff) {
                    this.fail("unpaired surrogate escape");
                } else {
                    out += String.fromCharCode(unit);
                }
            } else this.fail("invalid escape sequence");
        }
    }

    private parseNumber(): JValue {
        const start = this.pos;
        const negative = this.text[this.pos] === "-";
        if (negative) ++this.pos;
        const digit = (at: number) => this.text[at] >= "0" && this.text[at] <= "9";
        if (this.pos >= this.text.length || !digit(this.pos)) this.fail("invalid number");
        if (this.text[this.pos] === "0") {
            ++this.pos;
            if (digit(this.pos)) this.fail("leading zeros are not allowed");
        } else {
            while (digit(this.pos)) ++this.pos;
        }
        let integral = true;
        if (this.text[this.pos] === ".") {
            integral = false;
            ++this.pos;
            if (!digit(this.pos)) this.fail("expected digits after decimal point");
            while (digit(this.pos)) ++this.pos;
        }
        if (this.text[this.pos] === "e" || this.text[this.pos] === "E") {
            integral = false;
            ++this.pos;
            if (this.text[this.pos] === "+" || this.text[this.pos] === "-") ++this.pos;
            if (!digit(this.pos)) this.fail("expected digits in exponent");
            while (digit(this.pos)) ++this.pos;
        }
        const token = this.text.slice(start, this.pos);
        if (integral) {
            const wide = BigInt(token);
            // -0 keeps its sign as a double; beyond-int64 degrades to double
            // (core/base parse contract, mirrored token for token).
            if (negative && wide === 0n) return { k: "dbl", v: -0 };
            if (wide >= I64_MIN && wide <= I64_MAX) return { k: "int", raw: token };
        }
        return { k: "dbl", v: Number(token) };
    }
}

export function parseJson(text: string, origin: string): JValue {
    return new Parser(text, origin).parseDocument();
}

// ---------------------------------------------------------------- writing

const CONTROL_ESCAPES = new Map<string, string>([
    ['"', '\\"'],
    ["\\", "\\\\"],
    ["\n", "\\n"],
    ["\t", "\\t"],
    ["\r", "\\r"],
    ["\b", "\\b"],
    ["\f", "\\f"],
]);

export function escapeJsonString(text: string): string {
    let out = '"';
    for (let i = 0; i < text.length; ++i) {
        const c = text[i];
        const mapped = CONTROL_ESCAPES.get(c);
        if (mapped !== undefined) out += mapped;
        else if (c.charCodeAt(0) < 0x20)
            out += "\\u00" + c.charCodeAt(0).toString(16).padStart(2, "0");
        else out += c; // UTF-8 passes through untouched (writer contract)
    }
    return out + '"';
}

// Doubles: shortest round-trip digits (toExponential() == dragonbox by the
// uniqueness of shortest-with-even-ties), rendered with the
// std::to_chars(general) selection rule — fixed or scientific, whichever is
// shorter, ties to fixed; integer-valued fixed forms expand EXACTLY.
export function formatDouble(value: number): string {
    if (!isFinite(value)) return "null"; // JSON has no NaN/Inf
    if (value === 0) return Object.is(value, -0) ? "-0" : "0";
    const magnitude = Math.abs(value);
    const parts = magnitude.toExponential().split("e");
    const digits = parts[0].replace(".", "");
    const ndigits = digits.length;
    const x = parseInt(parts[1], 10); // power-of-ten (scientific) exponent
    const sign = value < 0 ? "-" : "";

    const expDigits = x <= -100 || x >= 100 ? 3 : 2;
    const sciLen = (ndigits === 1 ? 1 : ndigits + 1) + 2 + expDigits;
    const fixedLen = x >= ndigits - 1 ? x + 1 : x >= 0 ? ndigits + 1 : ndigits + 1 - x;

    if (fixedLen <= sciLen) {
        if (x >= ndigits - 1) return sign + BigInt(magnitude).toString(); // exact expansion
        if (x >= 0) return sign + digits.slice(0, x + 1) + "." + digits.slice(x + 1);
        return sign + "0." + "0".repeat(-x - 1) + digits;
    }
    let out = sign + digits[0];
    if (ndigits > 1) out += "." + digits.slice(1);
    out += "e" + (x < 0 ? "-" : "+") + String(x < 0 ? -x : x).padStart(2, "0");
    return out;
}

export function dumpJson(value: JValue): string {
    switch (value.k) {
        case "null":
            return "null";
        case "bool":
            return value.v ? "true" : "false";
        case "int":
            return value.raw;
        case "dbl":
            return formatDouble(value.v);
        case "str":
            return escapeJsonString(value.v);
        case "arr":
            return "[" + value.v.map(dumpJson).join(",") + "]";
        case "obj":
            return (
                "{" +
                value.v.map(([key, item]) => escapeJsonString(key) + ":" + dumpJson(item)).join(",") +
                "}"
            );
    }
}

// ------------------------------------------------------------ construction

export function jStr(v: string): JValue {
    return { k: "str", v };
}

export function jBool(v: boolean): JValue {
    return { k: "bool", v };
}

// Small known-safe integers only (format/envelope versions, tuple sizes).
export function jInt(v: number): JValue {
    return { k: "int", raw: String(v) };
}

export function jArr(v: JValue[]): JValue {
    return { k: "arr", v };
}

export function jObj(pairs: [string, JValue][]): JObject {
    return { k: "obj", v: pairs };
}

export function findKey(value: JValue, key: string): JValue | null {
    if (value.k !== "obj") return null;
    for (const [k, v] of value.v) if (k === key) return v;
    return null;
}
