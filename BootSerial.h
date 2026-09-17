/*
 * BootSerial.h — ToyBoot COM1（PR-BOOT-log-uart）
 *
 * 命名：Boot 前缀 + Serial + 动词（见开发命名规范 §2.1）。
 */
#ifndef TOY_BOOT_SERIAL_H
#define TOY_BOOT_SERIAL_H

#include <Uefi.h>

void EFIAPI BootSerialInitialize(void);
int  EFIAPI BootSerialPresent(void);
void EFIAPI BootSerialWrite(const char *Text);
void EFIAPI BootSerialPrintf(CONST CHAR8 *Fmt, ...);

#endif
