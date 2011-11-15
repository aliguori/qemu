QEMU User space emulator
========================

Supported Operating Systems
---------------------------

The following OS are supported in user space emulation:

 * Linux (referred as qemu-linux-user)
 * Mac OS X/Darwin (referred as qemu-darwin-user)
 * BSD (referred as qemu-bsd-user)

Linux User space emulator
-------------------------

### Quick Start

In order to launch a Linux process, QEMU needs the process executable
itself and all the target (x86) dynamic libraries used by it.

 * On x86, you can just try to launch any process by using the native libraries:

       qemu-i386 -L / /bin/ls
   
   -L / tells that the x86 dynamic linker must be searched with a / prefix.

 * Since QEMU is also a linux process, you can launch qemu with qemu (NOTE: you
   can only do that if you compiled QEMU from the sources):

       qemu-i386 -L / qemu-i386 -L / /bin/ls

 * On non x86 CPUs, you need first to download at least an x86 glibc
   (*qemu-runtime-i386-XXX-.tar.gz* on the QEMU web page). Ensure that
   *LD_LIBRARY_PATH* is not set:

       unset LD_LIBRARY_PATH

   Then you can launch the precompiled *ls* x86 executable:

       qemu-i386 tests/i386/ls

   You can look at *scripts/qemu-binfmt-conf.sh* so that QEMU is automatically
   launched by the Linux kernel when you try to launch x86 executables. It
   requires the *binfmt_misc* module in the Linux kernel.

 * The x86 version of QEMU is also included. You can try weird things such as:

       qemu-i386 /usr/local/qemu-i386/bin/qemu-i386 \
               /usr/local/qemu-i386/bin/ls-i386

### Wine launch

 1. Ensure that you have a working QEMU with the x86 glibc distribution (see
    previous section). In order to verify it, you must be able to do:

        qemu-i386 /usr/local/qemu-i386/bin/ls-i386

 2. Download the binary x86 Wine install (*qemu-XXX-i386-wine.tar.gz* on the
    QEMU web page).

 3. Configure Wine on your account. Look at the provided script
    */usr/local/qemu-i386/bin/wine-conf.sh*. Your previous
    *$HOME/.wine* directory is saved to *$HOME.wine.org*.

 4. Then you can try the example *putty.exe*:

        qemu-i386 /usr/local/qemu-i386/wine/bin/wine \
                  /usr/local/qemu-i386/wine/c/Program\ Files/putty.exe

### Command line options

    usage: qemu-i386 [-h] [-d] [-L path] [-s size] [-cpu model] [-g port] [-B offset] [-R size] program [arguments...]

 * -h

Print the help

 * -L path

Set the x86 elf interpreter prefix (default=/usr/local/qemu-i386)

 * -s size

Set the x86 stack size in bytes (default=524288)

 * -cpu model

Select CPU model (-cpu ? for list and additional feature selection)

 * -ignore-environment

Start with an empty environment. Without this option, the initial environment
is a copy of the caller's environment.

 * -E *var=value*

Set environment *var* to *value*.

 * -U *var*

Remove *var* from the environment.

 * -B offset

Offset guest address by the specified number of bytes.  This is useful when
the address region required by guest applications is reserved on the host.

This option is currently only supported on some hosts.

 * -R size

Pre-allocate a guest virtual address space of the given size (in bytes).
"G", "M", and "k" suffixes may be used when specifying the size.

Debug options:

 * -d

Activate log (logfile=/tmp/qemu.log)

 * -p pagesize

Act as if the host page size was 'pagesize' bytes

 * -g port

Wait gdb connection to port

 * -singlestep

Run the emulation in single step mode.

Environment variables:

 * QEMU_STRACE

Print system calls and arguments similar to the 'strace' program (NOTE: the
actual 'strace' program will not work because the user space emulator hasn't
implemented ptrace).  At the moment this is incomplete.  All system calls that
don't have a specific argument format are printed with information for six
arguments.  Many flag-style arguments don't have decoders and will show up as
numbers.

### Other binaries

#### qemu-alpha

TODO.

#### qemu-armeb

TODO.

#### qemu-arm

*qemu-arm* is also capable of running ARM "Angel" semihosted ELF binaries (as
implemented by the arm-elf and arm-eabi Newlib/GDB configurations), and
arm-uclinux bFLT format binaries.

#### qemu-m68k

*qemu-m68k* is capable of running semihosted binaries using the BDM
(m5xxx-ram-hosted.ld) or m68k-sim (sim.ld) syscall interfaces, and coldfire
uClinux bFLT format binaries.

The binary format is detected automatically.

#### qemu-cris

TODO.

#### qemu-i386/qemu-x86_64

TODO.

#### qemu-microblaze

TODO.

#### qemu-mips/qemu-mipsel

TODO.

#### qemu-ppc64abi32/qemu-ppc64/qemu-ppc

TODO.

#### qemu-sh4eb/qemu-sh4

TODO.

#### qemu-sparc

*qemu-sparc* can execute Sparc32 binaries (Sparc32 CPU, 32 bit ABI).

*qemu-sparc32plus* can execute Sparc32 and SPARC32PLUS binaries (Sparc64 CPU, 32
bit ABI).

*qemu-sparc64* can execute some Sparc64 (Sparc64 CPU, 64 bit ABI) and
SPARC32PLUS binaries (Sparc64 CPU, 32 bit ABI).

Mac OS X/Darwin User space emulator
-----------------------------------

### Mac OS X/Darwin Status

 - target x86 on x86: Most apps (Cocoa and Carbon too) works. [1]
 - target PowerPC on x86: Not working as the ppc commpage can't be mapped (yet!)
 - target PowerPC on PowerPC: Most apps (Cocoa and Carbon too) works. [1]
 - target x86 on PowerPC: most utilities work. Cocoa and Carbon apps are not
   yet supported.

[1] If you're host commpage can be executed by qemu.

### Mac OS X/Darwin Quick Start

In order to launch a Mac OS X/Darwin process, QEMU needs the process executable
itself and all the target dynamic libraries used by it. If you don't have the
FAT libraries (you're running Mac OS X/ppc) you'll need to obtain it from a Mac
OS X CD or compile them by hand.

 * On x86, you can just try to launch any process by using the native libraries:

       qemu-i386 /bin/ls

   or to run the ppc version of the executable:

       qemu-ppc /bin/ls

 * On ppc, you'll have to tell qemu where your x86 libraries (and dynamic
   linker) are installed:

       qemu-i386 -L /opt/x86_root/ /bin/ls

   *-L /opt/x86_root/* tells that the dynamic linker (dyld) path is in
   */opt/x86_root/usr/bin/dyld*.

### Mac OS X/Darwin Command line options

    usage: qemu-i386 [-h] [-d] [-L path] [-s size] program [arguments...]

 * -h

Print the help

 * -L path

Set the library root path (default=/)

 * -s size

Set the stack size in bytes (default=524288)

Debug options:

 * -d

Activate log (logfile=/tmp/qemu.log)

 * -p pagesize

Act as if the host page size was 'pagesize' bytes

 * -singlestep

Run the emulation in single step mode.

BSD User space emulator
-----------------------

### BSD Status

 * target Sparc64 on Sparc64: Some trivial programs work.

### BSD Quick Start

In order to launch a BSD process, QEMU needs the process executable itself and
all the target dynamic libraries used by it.

 * On Sparc64, you can just try to launch any process by using the native
   libraries:

       qemu-sparc64 /bin/ls

### BSD Command line options

    usage: qemu-sparc64 [-h] [-d] [-L path] [-s size] [-bsd type] program [arguments...]

 * -h

Print the help

 * -L path

Set the library root path (default=/)

 * -s size

Set the stack size in bytes (default=524288)

 * -ignore-environment

Start with an empty environment. Without this option, the initial environment
is a copy of the caller's environment.

 * -E *var*=*value*

Set environment *var* to *value*.

 * -U *var*

Remove *var* from the environment.

 * -bsd type

Set the type of the emulated BSD Operating system. Valid values are FreeBSD,
NetBSD and OpenBSD (default).

__Debug options__:

 * -d

Activate log (logfile=/tmp/qemu.log)

 * -p pagesize

Act as if the host page size was 'pagesize' bytes

 * -singlestep

Run the emulation in single step mode.
