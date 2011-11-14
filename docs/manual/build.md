Compilation from the sources
============================

Linux/Unix
----------

First you must decompress the sources:

    cd /tmp
    tar zxvf qemu-x.y.z.tar.gz
    cd qemu-x.y.z

Then you configure QEMU and build it (usually no options are needed):

    ./configure
    make

Then type as root user:

    make install

to install QEMU in */usr/local*.

Windows
-------

 1. Install the current versions of MSYS and [MinGW](http://www.mingw.org/). You
    can find detailed installation instructions in the download section and the
    FAQ.

 2. Download the MinGW development library of
    [SDL 1.2.x](http://www.libsdl.org). Unpack it in a temporary place and edit
    the *sdl-config* script so that it gives the correct SDL directory when
    invoked.

 3. Install the MinGW version of zlib and make sure *zlib.h* and *libz.dll.a*
    are in MinGW's default header and linker search paths.

 4. Extract the current version of QEMU.

 5. Start the MSYS shell (file *msys.bat*).

 6. Change to the QEMU directory. Launch *./configure* and *make*.  If you have
    problems using SDL, verify that *sdl-config* can be launched from the MSYS
    command line.

 7. You can install QEMU in *Program Files/Qemu* by typing *make install*.
    Don't forget to copy *SDL.dll* in *Program Files/Qemu*.

Cross compilation for Windows with Linux
----------------------------------------

 1. Install the MinGW cross compilation tools available at
    [MinGW](http://www.mingw.org/).

 2. Download the MinGW development library of
    [SDL 1.2.x](http://www.libsdl.org). Unpack it in a temporary place and
    edit the *sdl-config* script so that it gives the correct SDL directory
    when invoked.  Set up the *PATH* environment variable so that *sdl-config*
    can be launched by the QEMU configuration script.

 3. Install the MinGW version of zlib and make sure *zlib.h* and *libz.dll.a*
    are in MinGW's default header and linker search paths.

 4. Configure QEMU for Windows cross compilation:

        PATH=/usr/i686-pc-mingw32/sys-root/mingw/bin:$PATH ./configure --cross-prefix='i686-pc-mingw32-'

    The example assumes *sdl-config* is installed under
    */usr/i686-pc-mingw32/sys-root/mingw/bin* and MinGW cross compilation tools
    have names like *i686-pc-mingw32-gcc* and *i686-pc-mingw32-strip*. We set
    the *PATH* environment variable to ensure the MinGW version of *sdl-config*
    is used and use --cross-prefix to specify the name of the cross compiler.

    You can also use --prefix to set the Win32 install path which defaults to
     *c:/Program Files/Qemu*.

    Under Fedora Linux, you can run:

        yum -y install mingw32-gcc mingw32-SDL mingw32-zlib

    to get a suitable cross compilation environment.

 5.  You can install QEMU in the installation directory by typing *make
     install*. Don't forget to copy *SDL.dll* and *zlib1.dll* into the b
     installation directory.

Wine can be used to launch the resulting qemu.exe compiled for Win32.

Mac OS X
--------

The Mac OS X patches are not fully merged in QEMU, so you should look at the
QEMU mailing list archive to have all the necessary information.

Make targets
------------

 * make
 * make all

Make everything which is typically needed.

 * install

TODO

 * install-doc

TODO

 * make clean

Remove most files which were built during make.

 * make distclean

Remove everything which was built during make.

 * make dvi
 * make html
 * make info
 * make pdf

Create documentation in dvi, html, info or pdf format.

 * make cscope

TODO

 * make defconfig

(Re-)create some build configuration files. User made changes will be
overwritten.

 * tar
 * tarbin

TODO
