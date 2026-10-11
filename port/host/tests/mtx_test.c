#include <dolphin.h>
#include <math.h>
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int near_(f32 a, f32 b) { return fabsf(a - b) < 1e-4f; }
int main(void)
{
    Mtx a, b, c, inv, id; Vec v = {1, 2, 3}, o, ax = {0, 0, 2};
    PSMTXRotRad(a, 'y', 0.7f); PSMTXTrans(b, 4, -5, 6);
    PSMTXConcat(a, b, c);
    CHECK(PSMTXInverse(c, inv));
    PSMTXConcat(c, inv, id);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) CHECK(near_(id[i][j], i == j));
    PSMTXMultVec(b, &v, &o); CHECK(near_(o.x, 5) && near_(o.y, -3) && near_(o.z, 9));
    PSMTXRotAxisRad(a, &ax, 1.234f); PSMTXRotRad(b, 'z', 1.234f);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) CHECK(near_(a[i][j], b[i][j]));
    Vec x = {1, 0, 0}, y = {0, 1, 0}, z; PSVECCrossProduct(&x, &y, &z); CHECK(near_(z.z, 1));
    Point3d pos = {0, 0, 10}, tgt = {0, 0, 0}; Vec up = {0, 1, 0}; Mtx lk;
    C_MTXLookAt(lk, &pos, &up, &tgt); Vec p = {0, 0, 0}, q; PSMTXMultVec(lk, &p, &q); CHECK(near_(q.z, -10));
    Mtx44 pr; C_MTXPerspective(pr, 60, 1.333f, 1, 100); CHECK(near_(pr[3][2], -1));
    ROMtx ro; PSMTXReorder(c, ro); Vec s2, s1 = {7, 8, 9}; PSMTXROMultVecArray(ro, &s1, &s2, 1); PSMTXMultVec(c, &s1, &o);
    CHECK(near_(s2.x, o.x) && near_(s2.y, o.y) && near_(s2.z, o.z));
    printf(fails ? "mtx_test: %d FAILED\n" : "mtx_test: all passed\n", fails);
    return fails != 0;
}
