#pragma once
/*
 * FAT32 USB drives for WiiDrop: raw sector access through Aroma's Mocha (libmocha), the FAT32 file
 * system from FatFs, and a "usb:" device for the C library, so the rest of the app can simply use
 * paths like usb:/wbfs/Game [ID]/ID.wbfs with fopen, rename, mkdir, opendir, stat and statvfs.
 *
 * Only FAT32 is accepted (what USB Loader GX and Nintendont use). Drives with another file system,
 * such as a Wii U-formatted drive, are only read to check what they are, never written.
 */
#include <stddef.h>
#include <stdint.h>

/* Find a FAT32 USB drive and make it available as usb:/. 0 on success, else err says why. */
int usbfs_mount(char *err, size_t errlen);
void usbfs_unmount(void);
int usbfs_mounted(void);
