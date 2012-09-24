# LDT - Linux Driver Template

LDT project is useful for Linux driver development beginners and as starting point for a new drivers. 
The driver uses following Linux facilities: 
module, platform driver, file operations (read/write, mmap, ioctl, blocking and nonblocking mode, polling), kfifo, completion, interrupt, tasklet, work, kthread, timer, misc device, proc fs, UART 0x3f8, HW loopback, SW loopback.

## Usage:

Just run

git clone git://github.com/makelinux/ldt.git && cd ldt && ./ldt-test

and explore sources.

## Files:

Main source file of LDT: 
**[ldt.c](https://github.com/makelinux/ldt/blob/master/ldt.c)**

Test script, run it: **[ldt-test](https://github.com/makelinux/ldt/blob/master/ldt-test)**

Generic testing utility for Device I/O: **[dio.c](https://github.com/makelinux/ldt/blob/master/dio.c)**

Browse the rest of source: https://github.com/makelinux/ldt/


## Compiled and tested on Linux versions:

v3.6-rc5 

3.2.0-30-generic-pae (Ubuntu 12.04 LTS)

2.6.38-11-generic (Ubuntu 11.04)

v2.6.37-rc8

v2.6.36-rc8

### Failed compilation with:

v2.6.35-rc6 failed because of DEFINE_FIFO

<img src="http://const.homelinux.net/1.png">

<img src="http://www.android.com/images/brand/android_app_on_play_large.png">
--