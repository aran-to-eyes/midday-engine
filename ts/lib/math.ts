// midday/math — pooled math objects for sim scripts (spec §7: math types are
// pooled/reusable so steady-state ticks allocate ZERO GC bytes — pinned by
// the bindings.* zero-alloc fixtures). Every operation mutates `this` and
// returns it; nothing here ever allocates after pool warmup.
//
// Determinism scope: IEEE-exact arithmetic only (+ - * / and Math.sqrt are
// correctly rounded everywhere). Transcendentals (sin/cos/...) are LIBM-
// BOUND (core/math/README.md) and arrive through the engine bindings, not
// here — a pooled quat is composed from components or engine-provided data.

export class Vec3 {
    x = 0;
    y = 0;
    z = 0;

    set(x: number, y: number, z: number): this {
        this.x = x;
        this.y = y;
        this.z = z;
        return this;
    }

    copy(v: Vec3): this {
        return this.set(v.x, v.y, v.z);
    }

    add(v: Vec3): this {
        return this.set(this.x + v.x, this.y + v.y, this.z + v.z);
    }

    sub(v: Vec3): this {
        return this.set(this.x - v.x, this.y - v.y, this.z - v.z);
    }

    scale(s: number): this {
        return this.set(this.x * s, this.y * s, this.z * s);
    }

    addScaled(v: Vec3, s: number): this {
        return this.set(this.x + v.x * s, this.y + v.y * s, this.z + v.z * s);
    }

    /** this = a x b (safe when this aliases a or b). */
    cross(a: Vec3, b: Vec3): this {
        return this.set(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        );
    }

    dot(v: Vec3): number {
        return this.x * v.x + this.y * v.y + this.z * v.z;
    }

    lengthSquared(): number {
        return this.dot(this);
    }

    length(): number {
        return Math.sqrt(this.lengthSquared());
    }

    /** Zero vectors stay zero (no NaN escapes into the sim). */
    normalize(): this {
        const len = this.length();
        return len > 0 ? this.scale(1 / len) : this;
    }
}

export class Quat {
    x = 0;
    y = 0;
    z = 0;
    w = 1;

    set(x: number, y: number, z: number, w: number): this {
        this.x = x;
        this.y = y;
        this.z = z;
        this.w = w;
        return this;
    }

    copy(q: Quat): this {
        return this.set(q.x, q.y, q.z, q.w);
    }

    /** this = a * b (Hamilton product; safe when this aliases a or b). */
    multiply(a: Quat, b: Quat): this {
        return this.set(
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        );
    }

    /** out = this rotated v (unit quaternion; out may alias v). */
    rotate(out: Vec3, v: Vec3): Vec3 {
        // t = 2 q x v; v' = v + w t + q x t — arithmetic only, no trig.
        const tx = 2 * (this.y * v.z - this.z * v.y);
        const ty = 2 * (this.z * v.x - this.x * v.z);
        const tz = 2 * (this.x * v.y - this.y * v.x);
        return out.set(
            v.x + this.w * tx + this.y * tz - this.z * ty,
            v.y + this.w * ty + this.z * tx - this.x * tz,
            v.z + this.w * tz + this.x * ty - this.y * tx,
        );
    }
}

// Fixed-slot object pool: take() hands out pre-built objects, reset()
// rewinds the cursor each tick. Growth happens only while a tick needs more
// objects than any tick before it — steady state allocates nothing.
export class Pool<T> {
    private readonly items: T[] = [];
    private used = 0;

    constructor(private readonly make: () => T) {}

    take(): T {
        if (this.used === this.items.length) this.items.push(this.make());
        return this.items[this.used++];
    }

    /** Call once per tick, after the math is done. */
    reset(): void {
        this.used = 0;
    }

    get allocated(): number {
        return this.items.length;
    }
}

export const vec3Pool = new Pool(() => new Vec3());
export const quatPool = new Pool(() => new Quat());
