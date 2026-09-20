/*
 * BootVideo.c — PR-S-boot-1：GOP 选模与设分辨率
 *
 * 从 Boot.c 原样搬家；不改语义。
 */
#include <Protocol/GraphicsOutput.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#include "BootPrivate.h"

EFI_STATUS GetAndSetVideo(EFI_HANDLE ImageHandle, VIDEO_CONFIG *VideoConfig,
                                 BOOT_CONFIG *BootConfig) {
    EFI_STATUS                            Status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL          *Gop = NULL;
    UINTN                                 HandleCount = 0;
    EFI_HANDLE                            *HandleBuffer = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *ModeInfo = NULL;
    UINTN                                 InfoSize = 0;
    UINTN                                 BestMode = 0;
    UINTN                                 BestScore = 0;
    UINT32                                BestW = 0;
    UINT32                                BestH = 0;
    BOOLEAN                               InVm = IsVirtualMachine();
    BOOLEAN                               HasEdidTarget = FALSE;
    UINT32                                EdidW = 0;
    UINT32                                EdidH = 0;
    BOOLEAN                               HasCfgTarget = FALSE;
    UINT32                                CfgW = 0;
    UINT32                                CfgH = 0;
    BOOLEAN                               CfgMatched = FALSE;
    UINT32                                ModeCount = 0;

    BootSerialPrintf("Boot: GetAndSetVideo\n");

    if (BootConfig != NULL) {
        BootConfig->VideoModeCount = 0;
        BootConfig->VideoModePad = 0;
    }

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid,
                                     NULL, &HandleCount, &HandleBuffer);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = gBS->OpenProtocol(HandleBuffer[0], &gEfiGraphicsOutputProtocolGuid,
                               (VOID **)&Gop, ImageHandle, NULL,
                               EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    if (TryLoadDisplayPref(ImageHandle, &CfgW, &CfgH)) {
        HasCfgTarget = TRUE;
        BootDbg("ToyBoot: THEME.CFG Mode %dx%d\n", CfgW, CfgH);
    }

    if (!InVm) {
        if (!EFI_ERROR(TryGetEdidPreferred(ImageHandle, HandleBuffer[0], &EdidW, &EdidH))) {
            HasEdidTarget = TRUE;
            BootDbg("ToyBoot: Monitor EDID Preferred %dx%d\n", EdidW, EdidH);
        } else {
            BootSerialPrintf("ToyBoot: EDID Unavailable, Using Highest GOP Mode\n");
        }
    } else {
        BootDbg("ToyBoot: Virtual Machine Detected, Using QEMU-Friendly Mode Table\n");
    }

    BootDbg("Available Video Modes:\n");
    for (UINTN i = 0; i < Gop->Mode->MaxMode; i++) {
        Status = Gop->QueryMode(Gop, i, &InfoSize, &ModeInfo);
        if (EFI_ERROR(Status) || ModeInfo == NULL) {
            continue;
        }
        if (!IsModeUsable(ModeInfo)) {
            gBS->FreePool(ModeInfo);
            ModeInfo = NULL;
            continue;
        }

        {
            UINT32 W = ModeInfo->HorizontalResolution;
            UINT32 H = ModeInfo->VerticalResolution;
            UINTN  Score;
            UINT32 Mi;

            /* Settings 列表：去重 WxH */
            if (BootConfig != NULL && ModeCount < TOY_VIDEO_MODE_MAX) {
                for (Mi = 0; Mi < ModeCount; Mi++) {
                    if (BootConfig->VideoModes[Mi].Width == W &&
                        BootConfig->VideoModes[Mi].Height == H) {
                        break;
                    }
                }
                if (Mi == ModeCount) {
                    BootConfig->VideoModes[ModeCount].Width = W;
                    BootConfig->VideoModes[ModeCount].Height = H;
                    ModeCount++;
                }
            }

            /* 选模优先级：THEME.CFG →（无 CFG 时）EDID/QEMU 表 */
            if (HasCfgTarget && W == CfgW && H == CfgH) {
                Score = 5000000000ULL;
                CfgMatched = TRUE;
            } else if (HasCfgTarget) {
                Score = ScoreModeNative(W, H, CfgW, CfgH, TRUE);
            } else if (InVm) {
                Score = ScoreModeQemu(W, H);
            } else {
                Score = ScoreModeNative(W, H, EdidW, EdidH, HasEdidTarget);
            }

            BootDbg("  Mode %d: %dx%d (Score: %d)\n", i, W, H, Score);

            if (Score > BestScore) {
                BestScore = Score;
                BestMode = i;
                BestW = W;
                BestH = H;
            }
        }

        gBS->FreePool(ModeInfo);
        ModeInfo = NULL;
        InfoSize = 0;
    }

    if (BootConfig != NULL) {
        /* 列表序 ≠ 选模：按 EDID（或 VM 友好表）排，供 Settings 罗列 */
        SortVideoModesForSettings(BootConfig->VideoModes, ModeCount,
                                  InVm, HasEdidTarget, EdidW, EdidH);
        BootConfig->VideoModeCount = ModeCount;
        BootDbg("ToyBoot: %u Unique GOP Modes For Settings", ModeCount);
        if (!InVm && HasEdidTarget) {
            BootDbg(" (List: EDID %dx%d First)\n", EdidW, EdidH);
        } else {
            BootDbg("\n");
        }
    }

    if (HasCfgTarget && !CfgMatched) {
        if (SameAspectRatio(BestW, BestH, CfgW, CfgH)) {
            BootDbg("ToyBoot: Mode %dx%d Not In GOP; Nearest Same-Aspect %dx%d\n",
                  CfgW, CfgH, BestW, BestH);
        } else {
            BootDbg("ToyBoot: Mode %dx%d Not In GOP; Nearest %dx%d\n",
                  CfgW, CfgH, BestW, BestH);
        }
    } else if (!HasCfgTarget && HasEdidTarget &&
               (BestW != EdidW || BestH != EdidH)) {
        if (SameAspectRatio(BestW, BestH, EdidW, EdidH)) {
            BootDbg("ToyBoot: EDID %dx%d Not In GOP; Nearest Same-Aspect %dx%d\n",
                  EdidW, EdidH, BestW, BestH);
        } else {
            BootDbg("ToyBoot: EDID %dx%d Not In GOP; Nearest %dx%d\n",
                  EdidW, EdidH, BestW, BestH);
        }
    }

    if (BestScore == 0) {
        BootDbg("ToyBoot: No Usable GOP Mode Found\n");
        return EFI_NOT_FOUND;
    }

    /* 已是目标分辨率则勿 SetMode：QEMU+GTK 下改分辨率常会整机再复位一次 */
    {
        UINT32 CurW = 0;
        UINT32 CurH = 0;

        if (Gop->Mode != NULL && Gop->Mode->Info != NULL) {
            CurW = Gop->Mode->Info->HorizontalResolution;
            CurH = Gop->Mode->Info->VerticalResolution;
        }

        if (CurW == BestW && CurH == BestH) {
            BootDbg("ToyBoot: Already %dx%d, Skip SetMode\n", BestW, BestH);
        } else if (InVm) {
            /*
             * Guest reboot / QEMU Reset 不会重读宿主 run.sh 的 edid；若此处 SetMode
             * 改分辨率，GTK 跳变会再复位，固件又回到 edid 旧模式 → 无限重启。
             * 分辨率变更请退出 QEMU 后重新 ./run-split.sh（会按 rootfs THEME.CFG 设 edid）。
             */
            BootDbg("ToyBoot: Skip SetMode %dx%d -> %dx%d On VM (QEMU+GTK Loop)\n",
                  CurW, CurH, BestW, BestH);
            if (HasCfgTarget) {
                BootDbg("ToyBoot: THEME.CFG Wants %dx%d But GOP Is %dx%d\n",
                      CfgW, CfgH, CurW, CurH);
                BootDbg("ToyBoot: Quit QEMU Window, Then ./run-split.sh (EDID From THEME.CFG)\n");
            }
        } else {
            Status = Gop->SetMode(Gop, BestMode);
            if (EFI_ERROR(Status)) {
                BootSerialPrintf("ToyBoot: SetMode(%d) Failed: %r\n", BestMode, Status);
                return Status;
            }
        }
    }

    VideoConfig->FrameBufferBase = Gop->Mode->FrameBufferBase;
    VideoConfig->FrameBufferSize = Gop->Mode->FrameBufferSize;
    VideoConfig->HorizontalResolution = Gop->Mode->Info->HorizontalResolution;
    VideoConfig->VerticalResolution = Gop->Mode->Info->VerticalResolution;
    VideoConfig->PixelsPerScanLine = Gop->Mode->Info->PixelsPerScanLine;

    if (HasCfgTarget && CfgMatched &&
        VideoConfig->HorizontalResolution == CfgW &&
        VideoConfig->VerticalResolution == CfgH) {
        BootDbg("ToyBoot: Display %dx%d (THEME.CFG)\n",
              VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);
    } else if (HasCfgTarget &&
               (VideoConfig->HorizontalResolution != CfgW ||
                VideoConfig->VerticalResolution != CfgH)) {
        if (InVm) {
            BootDbg("ToyBoot: Display %dx%d (GOP; THEME.CFG %dx%d Not Applied — Relaunch QEMU)\n",
                  VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution,
                  CfgW, CfgH);
        } else {
            BootDbg("ToyBoot: Display %dx%d (Nearest To THEME.CFG %dx%d)\n",
                  VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution,
                  CfgW, CfgH);
        }
    } else if (InVm) {
        BootDbg("ToyBoot: Display %dx%d (QEMU/VM)\n",
              VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);
    } else if (HasEdidTarget &&
               VideoConfig->HorizontalResolution == EdidW &&
               VideoConfig->VerticalResolution == EdidH) {
        BootDbg("ToyBoot: Display %dx%d (EDID Native)\n", EdidW, EdidH);
    } else {
        BootDbg("ToyBoot: Display %dx%d (Hardware Best Match)\n",
              VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);
    }

    BootDbg("Selected Mode: %d, Final %dx%d\n",
          BestMode, VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);

    /* 设分辨率后清屏：黑底交给 Kernel 连续滚日志（PR-BOOT-log-uart） */
    if (Gop != NULL && Gop->Mode != NULL && Gop->Mode->Info != NULL) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black;
        UINTN W = Gop->Mode->Info->HorizontalResolution;
        UINTN H = Gop->Mode->Info->VerticalResolution;

        Black.Blue = 0;
        Black.Green = 0;
        Black.Red = 0;
        Black.Reserved = 0;
        (void)Gop->Blt(Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0, W, H, 0);
        BootSerialPrintf("Boot: GOP Cleared %ux%u\n", (UINT32)W, (UINT32)H);
    }

    if (HandleBuffer != NULL) {
        gBS->FreePool(HandleBuffer);
    }
    BootSerialPrintf("Boot: GetAndSetVideo Done\n");
    return EFI_SUCCESS;
}
