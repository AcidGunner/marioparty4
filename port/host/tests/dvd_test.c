#include <dolphin.h>
#include <stdio.h>
#include <string.h>
static int fails, cb_called;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void cb(s32 r, DVDFileInfo *fi) { cb_called = r; }
int main(void)
{
    DVDFileInfo fi; u8 buf[1024]; s32 e;
    DVDInit();
    e = DVDConvertPathToEntrynum("data/board.bin");   /* on disk: Data/Board.bin (case differs) */
    CHECK(e >= 0);
    CHECK(DVDConvertPathToEntrynum("data/board.bin") == e);
    CHECK(DVDConvertPathToEntrynum("data/nope.bin") == -1);
    CHECK(DVDFastOpen(e, &fi) && fi.length == 1000);
    memset(buf, 0xAA, sizeof(buf));
    DVDReadAsync(&fi, buf, 1024, 0, cb);               /* game rounds up to 32 B: past EOF is zero */
    CHECK(cb_called == 1024 && DVDGetCommandBlockStatus(&fi.cb) == 0);
    CHECK(buf[1023] == 0 && buf[999] != 0xAA);
    CHECK(DVDOpen("/meminfo.bin", &fi) && fi.length == 77);
    CHECK(!DVDOpen("/missing.bin", &fi));
    printf(fails ? "dvd_test: %d FAILED\n" : "dvd_test: all passed\n", fails);
    return fails != 0;
}
