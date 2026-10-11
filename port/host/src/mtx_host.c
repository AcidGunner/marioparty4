/* Portable C versions of the Gekko paired-single math routines the game calls.
 * (the decomp mtx sources use psq_l/psq_st asm and will not build with gcc.) */
#include <dolphin.h>
#include <math.h>

#define DEG2RAD(d) ((d) * 0.017453292519943295f)

void PSMTXIdentity(Mtx m)
{
    int i, j;
    for (i = 0; i < 3; i++) for (j = 0; j < 4; j++) m[i][j] = (i == j) ? 1.0f : 0.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst)
{
    int i, j;
    for (i = 0; i < 3; i++) for (j = 0; j < 4; j++) dst[i][j] = src[i][j];
}

void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab)
{
    Mtx t;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        t[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
    }
    PSMTXCopy(t, ab);
}

u32 PSMTXInverse(const Mtx s, Mtx inv)
{
    Mtx t;
    f32 det = s[0][0] * (s[1][1] * s[2][2] - s[1][2] * s[2][1])
            - s[0][1] * (s[1][0] * s[2][2] - s[1][2] * s[2][0])
            + s[0][2] * (s[1][0] * s[2][1] - s[1][1] * s[2][0]);
    if (det == 0.0f) return 0;
    f32 d = 1.0f / det;
    t[0][0] =  (s[1][1] * s[2][2] - s[1][2] * s[2][1]) * d;
    t[0][1] = -(s[0][1] * s[2][2] - s[0][2] * s[2][1]) * d;
    t[0][2] =  (s[0][1] * s[1][2] - s[0][2] * s[1][1]) * d;
    t[1][0] = -(s[1][0] * s[2][2] - s[1][2] * s[2][0]) * d;
    t[1][1] =  (s[0][0] * s[2][2] - s[0][2] * s[2][0]) * d;
    t[1][2] = -(s[0][0] * s[1][2] - s[0][2] * s[1][0]) * d;
    t[2][0] =  (s[1][0] * s[2][1] - s[1][1] * s[2][0]) * d;
    t[2][1] = -(s[0][0] * s[2][1] - s[0][1] * s[2][0]) * d;
    t[2][2] =  (s[0][0] * s[1][1] - s[0][1] * s[1][0]) * d;
    t[0][3] = -(t[0][0] * s[0][3] + t[0][1] * s[1][3] + t[0][2] * s[2][3]);
    t[1][3] = -(t[1][0] * s[0][3] + t[1][1] * s[1][3] + t[1][2] * s[2][3]);
    t[2][3] = -(t[2][0] * s[0][3] + t[2][1] * s[1][3] + t[2][2] * s[2][3]);
    PSMTXCopy(t, inv);
    return 1;
}

u32 PSMTXInvXpose(const Mtx s, Mtx invX)
{
    Mtx t;
    int i, j;
    if (!PSMTXInverse(s, t)) return 0;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) invX[i][j] = t[j][i];
        invX[i][3] = 0.0f;
    }
    return 1;
}

void PSMTXMultVec(const Mtx m, const Vec *s, Vec *d)
{
    Vec t;
    t.x = m[0][0] * s->x + m[0][1] * s->y + m[0][2] * s->z + m[0][3];
    t.y = m[1][0] * s->x + m[1][1] * s->y + m[1][2] * s->z + m[1][3];
    t.z = m[2][0] * s->x + m[2][1] * s->y + m[2][2] * s->z + m[2][3];
    *d = t;
}

void PSMTXMultVecSR(const Mtx m, const Vec *s, Vec *d)
{
    Vec t;
    t.x = m[0][0] * s->x + m[0][1] * s->y + m[0][2] * s->z;
    t.y = m[1][0] * s->x + m[1][1] * s->y + m[1][2] * s->z;
    t.z = m[2][0] * s->x + m[2][1] * s->y + m[2][2] * s->z;
    *d = t;
}

void PSMTXMultVecArray(const Mtx m, const Vec *src, Vec *dst, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) PSMTXMultVec(m, &src[i], &dst[i]);
}

/* ROMtx is the transposed (column-major, 4x3) layout: m[col][row]. */
void PSMTXReorder(const Mtx s, ROMtx d)
{
    int c, r;
    for (c = 0; c < 4; c++) for (r = 0; r < 3; r++) d[c][r] = s[r][c];
}

void PSMTXROMultVecArray(const ROMtx m, const Vec *src, Vec *dst, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) {
        Vec v = src[i];
        dst[i].x = m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0];
        dst[i].y = m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1];
        dst[i].z = m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2];
    }
}

void PSMTXRotRad(Mtx m, char axis, f32 rad)
{
    f32 s = sinf(rad), c = cosf(rad);
    PSMTXIdentity(m);
    switch (axis | 0x20) {
    case 'x': m[1][1] = c; m[1][2] = -s; m[2][1] = s; m[2][2] = c; break;
    case 'y': m[0][0] = c; m[0][2] = s; m[2][0] = -s; m[2][2] = c; break;
    case 'z': m[0][0] = c; m[0][1] = -s; m[1][0] = s; m[1][1] = c; break;
    }
}

void PSMTXRotAxisRad(Mtx m, const Vec *axis, f32 rad)
{
    Vec n;
    f32 s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    PSVECNormalize(axis, &n);
    m[0][0] = t * n.x * n.x + c;       m[0][1] = t * n.x * n.y - s * n.z; m[0][2] = t * n.x * n.z + s * n.y; m[0][3] = 0;
    m[1][0] = t * n.x * n.y + s * n.z; m[1][1] = t * n.y * n.y + c;       m[1][2] = t * n.y * n.z - s * n.x; m[1][3] = 0;
    m[2][0] = t * n.x * n.z - s * n.y; m[2][1] = t * n.y * n.z + s * n.x; m[2][2] = t * n.z * n.z + c;       m[2][3] = 0;
}

void PSMTXScale(Mtx m, f32 x, f32 y, f32 z)
{
    PSMTXIdentity(m);
    m[0][0] = x; m[1][1] = y; m[2][2] = z;
}

void PSMTXTrans(Mtx m, f32 x, f32 y, f32 z)
{
    PSMTXIdentity(m);
    m[0][3] = x; m[1][3] = y; m[2][3] = z;
}

/* ---- vectors ---- */
void PSVECAdd(const Vec *a, const Vec *b, Vec *r) { r->x = a->x + b->x; r->y = a->y + b->y; r->z = a->z + b->z; }
void PSVECSubtract(const Vec *a, const Vec *b, Vec *r) { r->x = a->x - b->x; r->y = a->y - b->y; r->z = a->z - b->z; }
void PSVECScale(const Vec *s, Vec *d, f32 k) { d->x = s->x * k; d->y = s->y * k; d->z = s->z * k; }
f32 PSVECDotProduct(const Vec *a, const Vec *b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
f32 PSVECSquareMag(const Vec *v) { return v->x * v->x + v->y * v->y + v->z * v->z; }
f32 PSVECMag(const Vec *v) { return sqrtf(PSVECSquareMag(v)); }
f32 PSVECDistance(const Vec *a, const Vec *b) { Vec d; PSVECSubtract(a, b, &d); return PSVECMag(&d); }

void PSVECNormalize(const Vec *s, Vec *u)
{
    f32 m = PSVECMag(s);
    if (m == 0.0f) { u->x = u->y = u->z = 0.0f; return; }
    PSVECScale(s, u, 1.0f / m);
}

void PSVECCrossProduct(const Vec *a, const Vec *b, Vec *r)
{
    Vec t;
    t.x = a->y * b->z - a->z * b->y;
    t.y = a->z * b->x - a->x * b->z;
    t.z = a->x * b->y - a->y * b->x;
    *r = t;
}

void C_VECHalfAngle(const Vec *a, const Vec *b, Vec *half)
{
    Vec a0 = { -a->x, -a->y, -a->z }, b0 = { -b->x, -b->y, -b->z }, h;
    PSVECNormalize(&a0, &a0);
    PSVECNormalize(&b0, &b0);
    PSVECAdd(&a0, &b0, &h);
    if (PSVECSquareMag(&h) <= 0.0f) { half->x = half->y = half->z = 0.0f; return; }
    PSVECNormalize(&h, half);
}

/* ---- projection / view ---- */
void C_MTXLookAt(Mtx m, const Point3d *pos, const Vec *up, const Point3d *target)
{
    Vec look, right, u;
    look.x = pos->x - target->x; look.y = pos->y - target->y; look.z = pos->z - target->z;
    PSVECNormalize(&look, &look);
    PSVECCrossProduct(up, &look, &right);
    PSVECNormalize(&right, &right);
    PSVECCrossProduct(&look, &right, &u);
    m[0][0] = right.x; m[0][1] = right.y; m[0][2] = right.z; m[0][3] = -(pos->x * right.x + pos->y * right.y + pos->z * right.z);
    m[1][0] = u.x;     m[1][1] = u.y;     m[1][2] = u.z;     m[1][3] = -(pos->x * u.x + pos->y * u.y + pos->z * u.z);
    m[2][0] = look.x;  m[2][1] = look.y;  m[2][2] = look.z;  m[2][3] = -(pos->x * look.x + pos->y * look.y + pos->z * look.z);
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f)
{
    f32 cot = 1.0f / tanf(DEG2RAD(fovY) * 0.5f), tmp = 1.0f / (f - n);
    int i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) m[i][j] = 0.0f;
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = -n * tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][2] = -1.0f;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    int i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) m[i][j] = 0.0f;
    m[0][0] = 2.0f / (r - l); m[0][3] = -(r + l) / (r - l);
    m[1][1] = 2.0f / (t - b); m[1][3] = -(t + b) / (t - b);
    m[2][2] = -1.0f / (f - n); m[2][3] = -f / (f - n);
    m[3][3] = 1.0f;
}

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 sS, f32 sT, f32 tS, f32 tT)
{
    f32 cot = 1.0f / tanf(DEG2RAD(fovY) * 0.5f);
    m[0][0] = (cot / aspect) * sS; m[0][1] = 0; m[0][2] = -tS; m[0][3] = 0;
    m[1][0] = 0; m[1][1] = cot * sT; m[1][2] = -tT; m[1][3] = 0;
    m[2][0] = 0; m[2][1] = 0; m[2][2] = -1.0f; m[2][3] = 0;
}
