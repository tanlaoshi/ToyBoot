/*
 * BootVideoScore.c — PR-S-boot-1：选模打分 / Settings 排序
 *
 * 从 Boot.c 原样搬家；不改语义。
 */
#include <Library/BaseLib.h>

#include "BootPrivate.h"

/* 虚拟机（QEMU/KVM 等）用窗口友好表；真机选模见 GetAndSetVideo（THEME 优先） */
BOOLEAN IsVirtualMachine(VOID) {
    UINT32 Eax;
    UINT32 Ebx;
    UINT32 Ecx;
    UINT32 Edx;

    AsmCpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    return (Ecx & BIT31) != 0;
}

BOOLEAN IsModeUsable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info) {
    if (Info == NULL) {
        return FALSE;
    }
    if (Info->PixelFormat == PixelBltOnly) {
        return FALSE;
    }
    if (Info->HorizontalResolution < 640 || Info->VerticalResolution < 480) {
        return FALSE;
    }
    return TRUE;
}

UINTN ScoreModeQemu(UINT32 W, UINT32 H) {
    static const struct {
        UINT32 W;
        UINT32 H;
        UINTN  Score;
    } Preferred[] = {
        { 1280,  720, 3000 },
        { 1600,  900, 2900 },
        { 1440,  900, 2800 },
        { 1680, 1050, 2700 },
        { 1280,  800, 2650 },
        { 1280, 1024, 2600 },
        { 1920, 1080, 2000 },
        { 1600, 1200, 1900 },
        { 1024,  768, 1000 },
        {  800,  600,  500 },
    };
    UINTN p;

    for (p = 0; p < sizeof(Preferred) / sizeof(Preferred[0]); p++) {
        if (W == Preferred[p].W && H == Preferred[p].H) {
            return Preferred[p].Score;
        }
    }
    if (W <= 2560 && H <= 1600) {
        return (UINTN)(W * H) / 10000;
    }
    return 0;
}

BOOLEAN SameAspectRatio(UINT32 W, UINT32 H, UINT32 TargetW, UINT32 TargetH) {
    if (W == 0 || H == 0 || TargetW == 0 || TargetH == 0) {
        return FALSE;
    }
    /* W/H == Tw/Th  ⇔  W*Th == H*Tw（无浮点） */
    return ((UINT64)W * (UINT64)TargetH) == ((UINT64)H * (UINT64)TargetW);
}

STATIC UINTN ManhattanDist(UINT32 W, UINT32 H, UINT32 TargetW, UINT32 TargetH) {
    INT64 Dw = (INT64)W - (INT64)TargetW;
    INT64 Dh = (INT64)H - (INT64)TargetH;

    if (Dw < 0) {
        Dw = -Dw;
    }
    if (Dh < 0) {
        Dh = -Dh;
    }
    return (UINTN)Dw + (UINTN)Dh;
}

/*
 * 相对 Target 打分（精确 → 同宽高比就近 → 其它就近）。
 * 选模：Target = THEME.CFG（有 mode=）或 Auto 时的 EDID。
 * Settings 列表排序：Target = EDID（与 THEME 无关）。
 */
UINTN ScoreModeNative(UINT32 W, UINT32 H, UINT32 TargetW, UINT32 TargetH,
                             BOOLEAN HasTarget) {
    UINTN Area = (UINTN)W * (UINTN)H;
    UINTN Dist;

    if (!HasTarget) {
        if (W <= 7680 && H <= 4320) {
            return Area;
        }
        return 0;
    }
    if (W == TargetW && H == TargetH) {
        return 3000000000ULL + Area;
    }
    Dist = ManhattanDist(W, H, TargetW, TargetH);
    if (Dist > 100000) {
        Dist = 100000;
    }
    if (SameAspectRatio(W, H, TargetW, TargetH)) {
        return 2000000000ULL - Dist * 1000ULL + Area / 10000ULL;
    }
    return 1000000000ULL - Dist * 1000ULL + Area / 10000ULL;
}

/* Settings 选项顺序：EDID 精确 → 同宽高比 → 就近（VM 用 QEMU 友好序） */
VOID SortVideoModesForSettings(TOY_VIDEO_MODE *Modes, UINT32 Count,
                                      BOOLEAN InVm, BOOLEAN HasEdid,
                                      UINT32 EdidW, UINT32 EdidH) {
    UINT32 i;
    UINT32 j;

    if (Modes == NULL || Count < 2) {
        return;
    }
    for (i = 0; i + 1 < Count; i++) {
        for (j = i + 1; j < Count; j++) {
            UINTN Si;
            UINTN Sj;
            TOY_VIDEO_MODE Tmp;

            if (InVm) {
                Si = ScoreModeQemu(Modes[i].Width, Modes[i].Height);
                Sj = ScoreModeQemu(Modes[j].Width, Modes[j].Height);
            } else {
                Si = ScoreModeNative(Modes[i].Width, Modes[i].Height,
                                    EdidW, EdidH, HasEdid);
                Sj = ScoreModeNative(Modes[j].Width, Modes[j].Height,
                                    EdidW, EdidH, HasEdid);
            }
            if (Sj > Si) {
                Tmp = Modes[i];
                Modes[i] = Modes[j];
                Modes[j] = Tmp;
            }
        }
    }
}
