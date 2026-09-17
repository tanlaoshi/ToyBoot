/*
 * BootSerial.c — ToyBoot COM1 16550（PR-BOOT-log-uart）
 *
 * ExitBootServices 前可用；与 Kernel Serial 同口同波特率，无 Hal 依赖。
 * NUC USB-UART：短 THR 超时会丢字符（尤其 '\\n'）→ 行粘连；等 THR/TEMT。
 */
#include "BootSerial.h"
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>

#define BOOT_COM1 0x3F8u
/* 真机/桥接器 THR 可能慢；2000 pause 不够会覆盖未发完字节 */
#define BOOT_SERIAL_WAIT 1000000

static int sSerialOk;

static inline void BootSerialOut8(UINT16 Port, UINT8 Value) {
    __asm__ volatile ("outb %0, %1" : : "a"(Value), "Nd"(Port));
}

static inline UINT8 BootSerialIn8(UINT16 Port) {
    UINT8 Value;
    __asm__ volatile ("inb %1, %0" : "=a"(Value) : "Nd"(Port));
    return Value;
}

static void BootSerialWaitBit(UINT8 Mask) {
    int Timeout = BOOT_SERIAL_WAIT;

    while (Timeout-- && !(BootSerialIn8(BOOT_COM1 + 5) & Mask)) {
        __asm__ volatile ("pause");
    }
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
    if (!sSerialOk) {
        return;
    }
    /* LSR bit5 THR empty */
    BootSerialWaitBit(0x20);
    BootSerialOut8(BOOT_COM1, (UINT8)C);
}

void EFIAPI BootSerialWrite(const char *Text) {
    if (!Text) {
        return;
    }
    while (*Text) {
        /*
         * AsciiVSPrint / PrintLib 常把格式里的 '\\n' 先扩成 "\\r\\n"。
         * 若此处再对 '\\n' 补 '\\r' → "\\r\\r\\n"：QEMU 多空行，NUC/CoolTerm 粘行、
         * ToyKernel 横幅像没换行。裸 '\\n'（横幅 BootSerialWrite）仍在此补 '\\r'。
         */
        if (*Text == '\r') {
            Text++;
            continue;
        }
        if (*Text == '\n') {
            BootSerialPutChar('\r');
            BootSerialPutChar('\n');
            /* LSR bit6 transmitter empty — 行尾刷出，避免 NUC 粘行 */
            BootSerialWaitBit(0x40);
        } else {
            BootSerialPutChar(*Text);
        }
        Text++;
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
