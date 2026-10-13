/*
 * PS3 controller input for the GameCube PAD API (PADInit / PADRead / ...).
 *
 * Connected DualShock/Sixaxis pads are assigned to GameCube ports 0-3 in the order libio reports them.
 * PADRead() is called by the game from its vertical-retrace callback (see VIWaitForRetrace in sdk_host.c).
 *
 * Mapping (PS3 -> GameCube):
 *   Cross -> A        Circle -> B       Square -> X       Triangle -> Y
 *   Start -> Start    D-pad -> D-pad    Left stick -> main stick    Right stick -> C stick
 *   L1 / L2 -> L      R1 / R2 -> R      Select / L3 / R3 -> Z
 *
 * Written against PSL1GHT's io/pad.h (ioPadInit, ioPadGetInfo, ioPadGetData and the BTN_* / ANA_* fields) without
 * access to the toolchain, so field names are from memory: if it fails to compile, the error will point at them.
 */
#ifdef PORT_PS3

#include <dolphin.h>
#include <string.h>
#include <io/pad.h>

static int pad_inited;

static s8 stick_axis(int raw, int invert)
{
    int c = raw - 128; /* libio: 0..255, centre 128 */
    if (c > -14 && c < 14) return 0;
    c = c * 72 / 127;  /* GameCube sticks top out around +-72 */
    if (c > 72) c = 72;
    if (c < -72) c = -72;
    return (s8)(invert ? -c : c);
}

BOOL PADInit(void)
{
    if (!pad_inited) {
        ioPadInit(7);
        pad_inited = 1;
    }
    return TRUE;
}

BOOL PADReset(u32 mask) { return TRUE; }
void PADSetSpec(u32 spec) {}
void PADSetAnalogMode(u32 mode) {}
void PADControlMotor(s32 chan, u32 cmd) {}
void PADClamp(PADStatus *status) {}
void PADClampCircle(PADStatus *status) {}

u32 PADRead(PADStatus *status)
{
    padInfo info;
    int i, n = 0;
    memset(status, 0, sizeof(PADStatus) * PAD_CHANMAX);
    for (i = 0; i < PAD_CHANMAX; i++) status[i].err = PAD_ERR_NO_CONTROLLER;
    if (!pad_inited) return 0;
    ioPadGetInfo(&info);
    for (i = 0; i < MAX_PADS && n < PAD_CHANMAX; i++) {
        padData d;
        PADStatus *s;
        u16 b = 0;
        if (!info.status[i]) continue;
        memset(&d, 0, sizeof(d));
        ioPadGetData(i, &d);
        s = &status[n++];
        if (d.BTN_LEFT) b |= PAD_BUTTON_LEFT;
        if (d.BTN_RIGHT) b |= PAD_BUTTON_RIGHT;
        if (d.BTN_DOWN) b |= PAD_BUTTON_DOWN;
        if (d.BTN_UP) b |= PAD_BUTTON_UP;
        if (d.BTN_CROSS) b |= PAD_BUTTON_A;
        if (d.BTN_CIRCLE) b |= PAD_BUTTON_B;
        if (d.BTN_SQUARE) b |= PAD_BUTTON_X;
        if (d.BTN_TRIANGLE) b |= PAD_BUTTON_Y;
        if (d.BTN_START) b |= PAD_BUTTON_START;
        if (d.BTN_L1 || d.BTN_L2) b |= PAD_TRIGGER_L;
        if (d.BTN_R1 || d.BTN_R2) b |= PAD_TRIGGER_R;
        if (d.BTN_SELECT || d.BTN_L3 || d.BTN_R3) b |= PAD_TRIGGER_Z;
        s->button = b;
        s->stickX = stick_axis(d.ANA_L_H, 0);
        s->stickY = stick_axis(d.ANA_L_V, 1); /* PS3: down is larger; GC: up is positive */
        s->substickX = stick_axis(d.ANA_R_H, 0);
        s->substickY = stick_axis(d.ANA_R_V, 1);
        s->triggerL = (d.BTN_L1 || d.BTN_L2) ? 255 : 0;
        s->triggerR = (d.BTN_R1 || d.BTN_R2) ? 255 : 0;
        s->err = PAD_ERR_NONE;
    }
    return 0;
}

#endif /* PORT_PS3 */
