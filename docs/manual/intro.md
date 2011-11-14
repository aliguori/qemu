Introduction
============

QEMU is a FAST! processor emulator using dynamic translation to achieve good
emulation speed.

QEMU has two operating modes:

 * __Full system emulation__ In this mode, QEMU emulates a full system (for
   example a PC), including one or several processors and various peripherals.
   It can be used to launch different Operating Systems without rebooting the
   PC or to debug system code.

 * __User mode emulation__ In this mode, QEMU can launch processes compiled for
   one CPU on another CPU. It can be used to launch the
   [Wine](http://www.winehq.org) Windows API emulator or to ease
   cross-compilation and cross-debugging.

QEMU can run without an host kernel driver and yet gives acceptable
performance.

For system emulation, the following hardware targets are supported:

 * PC (x86 or x86_64 processor)
 * ISA PC (old style PC without PCI bus)
 * PREP (PowerPC processor)
 * G3 Beige PowerMac (PowerPC processor)
 * Mac99 PowerMac (PowerPC processor, in progress)
 * Sun4m/Sun4c/Sun4d (32-bit Sparc processor)
 * Sun4u/Sun4v (64-bit Sparc processor, in progress)
 * Malta board (32-bit and 64-bit MIPS processors)
 * MIPS Magnum (64-bit MIPS processor)
 * ARM Integrator/CP (ARM)
 * ARM Versatile baseboard (ARM)
 * ARM RealView Emulation/Platform baseboard (ARM)
 * Spitz, Akita, Borzoi, Terrier and Tosa PDAs (PXA270 processor)
 * Luminary Micro LM3S811EVB (ARM Cortex-M3)
 * Luminary Micro LM3S6965EVB (ARM Cortex-M3)
 * Freescale MCF5208EVB (ColdFire V2).
 * Arnewsh MCF5206 evaluation board (ColdFire V2).
 * Palm Tungsten|E PDA (OMAP310 processor)
 * N800 and N810 tablets (OMAP2420 processor)
 * MusicPal (MV88W8618 ARM processor)
 * Gumstix "Connex" and "Verdex" motherboards (PXA255/270).
 * Siemens SX1 smartphone (OMAP310 processor)
 * Syborg SVP base model (ARM Cortex-A8).
 * AXIS-Devboard88 (CRISv32 ETRAX-FS).
 * Petalogix Spartan 3aDSP1800 MMU ref design (MicroBlaze).
 * Avnet LX60/LX110/LX200 boards (Xtensa)

For user emulation, x86 (32 and 64 bit), PowerPC (32 and 64 bit), ARM,
MIPS (32 bit only), Sparc (32 and 64 bit), Alpha, ColdFire(m68k), CRISv32 and
MicroBlaze CPUs are supported.
