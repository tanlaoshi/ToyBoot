/*
 * BootSerial.c — ToyBoot COM1 16550（PR-BOOT-log-uart）
 *
 * ExitBootServices 前可用；与 Kernel Serial 同口同波特率，无 Hal 依赖。
 */
#include "BootSerial.h"
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>

#define BOOT_COM1 0x3F8u

static int sSerialOk;

static inline void BootSerialOut8(UINT16 Port, UINT8 Value) {
    __asm__ volatile ("outb %0, %1" : : "a"(Value), "Nd"(Port));
}

static inline UINT8 BootSerialIn8(UINT16 Port) {
    UINT8 Value;
    __asm__ volatile ("inb %1, %0" : "=a"(Value) : "Nd"(Port));
    return Value;
}

static int BootSerialProbe(void) {
    UINT8 A;
    UINT8 B;

    BootSerialOut8(BOOT_COM1 + 7, 0x55);
    A = BootSerialIn8(BOOT_COM1 + 7);
    BootSerialOut8(BOOT_COM1 + 7, 0xAA);
    B = BootSerialIn8(BOOT_COM1 + 7);
    return (A == 0x55 && B == 0xAA) ? 1 : 0;
}

void EFIAPI BootSerialInitialize(void) {
    sSerialOk = BootSerialProbe();
    if (!sSerialOk) {
        return;
    }
    BootSerialOut8(BOOT_COM1 + 1, 0x00);
    BootSerialOut8(BOOT_COM1 + 3, 0x80);
    BootSerialOut8(BOOT_COM1 + 0, 0x01); /* 115200 divisor low */
    BootSerialOut8(BOOT_COM1 + 1, 0x00);
    BootSerialOut8(BOOT_COM1 + 3, 0x03); /* 8N1 */
    BootSerialOut8(BOOT_COM1 + 2, 0xC7);
    BootSerialOut8(BOOT_COM1 + 4, 0x0B);
}

int EFIAPI BootSerialPresent(void) {
    return sSerialOk;
}

static void BootSerialPutChar(char C) {
    int Timeout;

    if (!sSerialOk) {
        return;
    }
    if (C == '\n') {
        BootSerialPutChar('\r');
    }
    Timeout = 2000;
    while (Timeout-- && !(BootSerialIn8(BOOT_COM1 + 5) & 0x20)) {
        __asm__ volatile ("pause");
    }
    BootSerialOut8(BOOT_COM1, (UINT8)C);
}

void EFIAPI BootSerialWrite(const char *Text) {
    if (!Text) {
        return;
    }
    while (*Text) {
        BootSerialPutChar(*Text++);
    }
}

/* EFIAPI 必须：X64 上 VA_LIST 与 PrintLib（MS ABI）对齐，否则 %d 等会 #PF */
void EFIAPI BootSerialPrintf(CONST CHAR8 *Fmt, ...) {
    CHAR8 Buf[256];
    VA_LIST Args;

    if (!Fmt) {
        return;
    }
    VA_START(Args, Fmt);
    AsciiVSPrint(Buf, sizeof(Buf), Fmt, Args);
    VA_END(Args);
    BootSerialWrite(Buf);
}
