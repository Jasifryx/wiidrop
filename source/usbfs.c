#include "usbfs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "fatfs/ff.h" /* (before diskio.h, which uses its types) */
#include "fatfs/diskio.h"

#if defined(__WIIU__)
#include <coreinit/ios.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <mocha/fsa.h>
#include <mocha/mocha.h>
#include <sys/iosupport.h>
#include <sys/statvfs.h>
#else
/* PC test build: the "drive" is a disk image file, and the devoptab types come from a small shim */
#include "usbfs_pc_shim.h"
#endif

#define SECTOR 512u
#define BOUNCE_SECTORS 2048u /* 1 MB: the most moved per request, through a 0x40-aligned buffer */

static FATFS gFs;
static bool gMounted;
static uint8_t *gBounce;

/* ------------------------------------------------------------------ the raw drive */

#if defined(__WIIU__)
static int gFsa = -1, gDev = -1;
static bool gMochaInit;
static const char *const kUsbDevices[] = {"/dev/usb01", "/dev/usb02"};

static void raw_close(void)
{
    if (gDev >= 0) FSAEx_RawCloseEx(gFsa, gDev);
    if (gFsa >= 0) IOS_Close(gFsa);
    gDev = gFsa = -1;
}

static bool raw_open(const char *path)
{
    gFsa = IOS_Open("/dev/fsa", IOS_OPEN_READWRITE);
    if (gFsa < 0) return false;
    if (Mocha_UnlockFSClientEx(gFsa) != MOCHA_RESULT_SUCCESS || FSAEx_RawOpenEx(gFsa, path, &gDev) < 0) {
        gDev = -1;
        raw_close();
        return false;
    }
    return true;
}

static bool raw_read(uint32_t sector, uint32_t count, void *buf)
{
    return FSAEx_RawReadEx(gFsa, buf, SECTOR, count, sector, gDev) >= 0;
}

static bool raw_write(uint32_t sector, uint32_t count, const void *buf)
{
    return FSAEx_RawWriteEx(gFsa, buf, SECTOR, count, sector, gDev) >= 0;
}

static void *alloc_aligned(size_t n)
{
    return memalign(0x40, n);
}
#else
static FILE *gImg;
const char *usbfs_test_image; /* set by the test program */

static void raw_close(void)
{
    if (gImg) fclose(gImg);
    gImg = NULL;
}

static bool raw_open(const char *path)
{
    (void)path;
    gImg = usbfs_test_image ? fopen(usbfs_test_image, "r+b") : NULL;
    return gImg != NULL;
}

static bool raw_read(uint32_t sector, uint32_t count, void *buf)
{
    return _fseeki64(gImg, (long long)sector * SECTOR, SEEK_SET) == 0 && fread(buf, SECTOR, count, gImg) == count;
}

static bool raw_write(uint32_t sector, uint32_t count, const void *buf)
{
    return _fseeki64(gImg, (long long)sector * SECTOR, SEEK_SET) == 0 && fwrite(buf, SECTOR, count, gImg) == count;
}

static void *alloc_aligned(size_t n)
{
    return malloc(n);
}

/* tests only: open the image without mounting (to format it) */
int usbfs_test_rawopen(void)
{
    if (!gBounce) gBounce = alloc_aligned((size_t)BOUNCE_SECTORS * SECTOR);
    return raw_open("image") ? 0 : -1;
}

void usbfs_test_rawclose(void)
{
    raw_close();
}
#endif

/* ------------------------------------------------------------------ FatFs disk glue */

DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return 0; /* opened in usbfs_mount() before FatFs ever asks */
}

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;
    return 0;
}

/* Mocha wants 0x40-aligned buffers: aligned ones go straight through, others via the bounce buffer. */
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if ((uint64_t)sector + count > 0xFFFFFFFFull) return RES_PARERR; /* Mocha addresses 32-bit sectors (2 TB) */
    while (count > 0) {
        UINT n = count < BOUNCE_SECTORS ? count : BOUNCE_SECTORS;
        if (((uintptr_t)buff & 0x3F) == 0) {
            if (!raw_read((uint32_t)sector, n, buff)) return RES_ERROR;
        } else {
            if (!raw_read((uint32_t)sector, n, gBounce)) return RES_ERROR;
            memcpy(buff, gBounce, (size_t)n * SECTOR);
        }
        buff += (size_t)n * SECTOR;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if ((uint64_t)sector + count > 0xFFFFFFFFull) return RES_PARERR;
    while (count > 0) {
        UINT n = count < BOUNCE_SECTORS ? count : BOUNCE_SECTORS;
        if (((uintptr_t)buff & 0x3F) == 0) {
            if (!raw_write((uint32_t)sector, n, buff)) return RES_ERROR;
        } else {
            memcpy(gBounce, buff, (size_t)n * SECTOR);
            if (!raw_write((uint32_t)sector, n, gBounce)) return RES_ERROR;
        }
        buff += (size_t)n * SECTOR;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK; /* writes go straight to the drive: nothing is held back */
    case GET_SECTOR_SIZE: *(WORD *)buff = SECTOR; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
#if !defined(__WIIU__)
    case GET_SECTOR_COUNT: /* tests format their disk image; the app never formats anything */
        _fseeki64(gImg, 0, SEEK_END);
        *(LBA_t *)buff = (LBA_t)(_ftelli64(gImg) / SECTOR);
        return RES_OK;
#endif
    default: return RES_PARERR; /* GET_SECTOR_COUNT is only for formatting, which WiiDrop never does */
    }
}

/* timestamps for new files */
DWORD get_fattime(void)
{
    int year, mon, mday, hour, min, sec;
#if defined(__WIIU__)
    OSCalendarTime ct;
    OSTicksToCalendarTime(OSGetTime(), &ct);
    year = ct.tm_year;
    mon = ct.tm_mon + 1;
    mday = ct.tm_mday;
    hour = ct.tm_hour;
    min = ct.tm_min;
    sec = ct.tm_sec;
#else
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    year = tm->tm_year + 1900;
    mon = tm->tm_mon + 1;
    mday = tm->tm_mday;
    hour = tm->tm_hour;
    min = tm->tm_min;
    sec = tm->tm_sec;
#endif
    if (year < 1980) year = 1980;
    return ((DWORD)(year - 1980) << 25) | ((DWORD)mon << 21) | ((DWORD)mday << 16) | ((DWORD)hour << 11) |
           ((DWORD)min << 5) | ((DWORD)sec >> 1);
}

/* ------------------------------------------------------------------ the usb: device */

typedef struct {
    FIL fil;
    bool append;
} UsbFile;

static int fr_errno(FRESULT fr)
{
    switch (fr) {
    case FR_OK: return 0;
    case FR_NO_FILE:
    case FR_NO_PATH: return ENOENT;
    case FR_EXIST: return EEXIST;
    case FR_DENIED: return EACCES;
    case FR_WRITE_PROTECTED: return EROFS;
    case FR_INVALID_NAME: return EINVAL;
    case FR_NOT_ENOUGH_CORE: return ENOMEM;
    case FR_TOO_MANY_OPEN_FILES: return EMFILE;
    case FR_LOCKED: return EBUSY;
    case FR_INVALID_OBJECT: return EBADF;
    default: return EIO;
    }
}

static int fail(struct _reent *r, FRESULT fr)
{
    r->_errno = fr_errno(fr);
    return -1;
}

/* "usb:/wbfs/x" -> "/wbfs/x" (FatFs has a single drive, so no drive number is needed) */
static const char *fat_path(const char *path)
{
    const char *colon = strchr(path, ':');
    const char *p = colon ? colon + 1 : path;
    return *p ? p : "/";
}

static bool is_root(const char *p)
{
    while (*p == '/') p++;
    return *p == '\0';
}

static void fill_stat(struct stat *st, const FILINFO *fno)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = (fno->fattrib & AM_DIR) ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_size = (off_t)fno->fsize;
    st->st_nlink = 1;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((fno->fdate >> 9) & 0x7F) + 80;
    tm.tm_mon = ((fno->fdate >> 5) & 0x0F) - 1;
    tm.tm_mday = fno->fdate & 0x1F;
    tm.tm_hour = (fno->ftime >> 11) & 0x1F;
    tm.tm_min = (fno->ftime >> 5) & 0x3F;
    tm.tm_sec = (fno->ftime & 0x1F) * 2;
    tm.tm_isdst = -1;
    st->st_mtime = st->st_atime = st->st_ctime = mktime(&tm);
}

static int usb_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode)
{
    (void)mode;
    UsbFile *f = fileStruct;
    memset(f, 0, sizeof(*f));
    BYTE m = 0;
    switch (flags & O_ACCMODE) {
    case O_RDONLY: m = FA_READ; break;
    case O_WRONLY: m = FA_WRITE; break;
    default: m = FA_READ | FA_WRITE; break;
    }
    if (flags & O_CREAT) {
        if (flags & O_EXCL) m |= FA_CREATE_NEW;
        else if (flags & O_TRUNC) m |= FA_CREATE_ALWAYS;
        else m |= FA_OPEN_ALWAYS;
    } else {
        m |= FA_OPEN_EXISTING;
    }
    FRESULT fr = f_open(&f->fil, fat_path(path), m);
    if (fr != FR_OK) return fail(r, fr);
    if ((flags & O_TRUNC) && !(flags & O_CREAT) && (m & FA_WRITE)) {
        fr = f_truncate(&f->fil);
        if (fr != FR_OK) {
            f_close(&f->fil);
            return fail(r, fr);
        }
    }
    f->append = (flags & O_APPEND) != 0;
    if (f->append) {
        fr = f_lseek(&f->fil, f_size(&f->fil));
        if (fr != FR_OK) {
            f_close(&f->fil);
            return fail(r, fr);
        }
    }
    return 0;
}

static int usb_close(struct _reent *r, void *fd)
{
    FRESULT fr = f_close(&((UsbFile *)fd)->fil);
    return fr == FR_OK ? 0 : fail(r, fr);
}

static ssize_t usb_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    UsbFile *f = fd;
    if (f->append) {
        FRESULT fr = f_lseek(&f->fil, f_size(&f->fil));
        if (fr != FR_OK) return fail(r, fr);
    }
    UINT done = 0;
    FRESULT fr = f_write(&f->fil, ptr, (UINT)len, &done);
    if (fr != FR_OK) return fail(r, fr);
    if (done == 0 && len > 0) {
        r->_errno = ENOSPC; /* FatFs reports a full drive as a short write */
        return -1;
    }
    return (ssize_t)done;
}

static ssize_t usb_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
    UINT done = 0;
    FRESULT fr = f_read(&((UsbFile *)fd)->fil, ptr, (UINT)len, &done);
    return fr == FR_OK ? (ssize_t)done : fail(r, fr);
}

static off_t usb_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
    FIL *fp = &((UsbFile *)fd)->fil;
    int64_t base = dir == SEEK_CUR ? (int64_t)f_tell(fp) : dir == SEEK_END ? (int64_t)f_size(fp) : 0;
    int64_t to = base + (int64_t)pos;
    if (to < 0) {
        r->_errno = EINVAL;
        return -1;
    }
    FRESULT fr = f_lseek(fp, (FSIZE_t)to);
    return fr == FR_OK ? (off_t)f_tell(fp) : fail(r, fr);
}

static int usb_fstat(struct _reent *r, void *fd, struct stat *st)
{
    (void)r;
    FIL *fp = &((UsbFile *)fd)->fil;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0666;
    st->st_size = (off_t)f_size(fp);
    st->st_nlink = 1;
    return 0;
}

static int usb_stat(struct _reent *r, const char *path, struct stat *st)
{
    const char *p = fat_path(path);
    if (is_root(p)) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0777;
        return 0;
    }
    FILINFO fno;
    FRESULT fr = f_stat(p, &fno);
    if (fr != FR_OK) return fail(r, fr);
    fill_stat(st, &fno);
    return 0;
}

static int usb_unlink(struct _reent *r, const char *path)
{
    FRESULT fr = f_unlink(fat_path(path));
    return fr == FR_OK ? 0 : fail(r, fr);
}

static int usb_rmdir(struct _reent *r, const char *path)
{
    FRESULT fr = f_unlink(fat_path(path)); /* FatFs removes empty folders with f_unlink too */
    if (fr == FR_DENIED) {
        r->_errno = ENOTEMPTY;
        return -1;
    }
    return fr == FR_OK ? 0 : fail(r, fr);
}

/* POSIX rename replaces an existing file; FatFs refuses, so make room first */
static int usb_rename(struct _reent *r, const char *oldName, const char *newName)
{
    const char *from = fat_path(oldName), *to = fat_path(newName);
    FRESULT fr = f_rename(from, to);
    if (fr == FR_EXIST) {
        FILINFO fno;
        if (f_stat(to, &fno) == FR_OK && !(fno.fattrib & AM_DIR)) {
            fr = f_unlink(to);
            if (fr == FR_OK) fr = f_rename(from, to);
        }
    }
    return fr == FR_OK ? 0 : fail(r, fr);
}

static int usb_mkdir(struct _reent *r, const char *path, int mode)
{
    (void)mode;
    FRESULT fr = f_mkdir(fat_path(path));
    return fr == FR_OK ? 0 : fail(r, fr);
}

static DIR_ITER *usb_diropen(struct _reent *r, DIR_ITER *dirState, const char *path)
{
    FRESULT fr = f_opendir((DIR *)dirState->dirStruct, fat_path(path));
    if (fr != FR_OK) {
        fail(r, fr);
        return NULL;
    }
    return dirState;
}

static int usb_dirreset(struct _reent *r, DIR_ITER *dirState)
{
    FRESULT fr = f_rewinddir((DIR *)dirState->dirStruct);
    return fr == FR_OK ? 0 : fail(r, fr);
}

static int usb_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat)
{
    FILINFO fno;
    FRESULT fr = f_readdir((DIR *)dirState->dirStruct, &fno);
    if (fr != FR_OK) return fail(r, fr);
    if (!fno.fname[0]) { /* end of the folder */
        r->_errno = ENOENT;
        return -1;
    }
#ifdef NAME_MAX
    snprintf(filename, NAME_MAX + 1, "%s", fno.fname);
#else
    snprintf(filename, 256, "%s", fno.fname);
#endif
    if (filestat) fill_stat(filestat, &fno);
    return 0;
}

static int usb_dirclose(struct _reent *r, DIR_ITER *dirState)
{
    FRESULT fr = f_closedir((DIR *)dirState->dirStruct);
    return fr == FR_OK ? 0 : fail(r, fr);
}

static int usb_statvfs(struct _reent *r, const char *path, struct statvfs *buf)
{
    (void)path;
    DWORD freeClusters = 0;
    FATFS *fs = NULL;
    FRESULT fr = f_getfree("", &freeClusters, &fs);
    if (fr != FR_OK) return fail(r, fr);
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = buf->f_frsize = (unsigned long)fs->csize * SECTOR;
    buf->f_blocks = fs->n_fatent - 2;
    buf->f_bfree = buf->f_bavail = freeClusters;
    buf->f_namemax = 255;
    return 0;
}

static int usb_ftruncate(struct _reent *r, void *fd, off_t len)
{
    FIL *fp = &((UsbFile *)fd)->fil;
    FSIZE_t keep = f_tell(fp);
    FRESULT fr = f_lseek(fp, (FSIZE_t)len);
    if (fr == FR_OK) fr = f_truncate(fp);
    if (fr == FR_OK) fr = f_lseek(fp, keep < (FSIZE_t)len ? keep : (FSIZE_t)len);
    return fr == FR_OK ? 0 : fail(r, fr);
}

static int usb_fsync(struct _reent *r, void *fd)
{
    FRESULT fr = f_sync(&((UsbFile *)fd)->fil);
    return fr == FR_OK ? 0 : fail(r, fr);
}

static devoptab_t gUsbDevoptab;

const devoptab_t *usbfs_devoptab(void)
{
    if (!gUsbDevoptab.name) {
        gUsbDevoptab.name = "usb";
        gUsbDevoptab.structSize = sizeof(UsbFile);
        gUsbDevoptab.open_r = usb_open;
        gUsbDevoptab.close_r = usb_close;
        gUsbDevoptab.write_r = usb_write;
        gUsbDevoptab.read_r = usb_read;
        gUsbDevoptab.seek_r = usb_seek;
        gUsbDevoptab.fstat_r = usb_fstat;
        gUsbDevoptab.stat_r = usb_stat;
        gUsbDevoptab.unlink_r = usb_unlink;
        gUsbDevoptab.rename_r = usb_rename;
        gUsbDevoptab.mkdir_r = usb_mkdir;
        gUsbDevoptab.dirStateSize = sizeof(DIR);
        gUsbDevoptab.diropen_r = usb_diropen;
        gUsbDevoptab.dirreset_r = usb_dirreset;
        gUsbDevoptab.dirnext_r = usb_dirnext;
        gUsbDevoptab.dirclose_r = usb_dirclose;
        gUsbDevoptab.statvfs_r = usb_statvfs;
        gUsbDevoptab.ftruncate_r = usb_ftruncate;
        gUsbDevoptab.fsync_r = usb_fsync;
        gUsbDevoptab.rmdir_r = usb_rmdir;
        gUsbDevoptab.lstat_r = usb_stat;
    }
    return &gUsbDevoptab;
}

/* ------------------------------------------------------------------ mounting */

static const char *fs_name(BYTE type)
{
    return type == FS_FAT12 ? "FAT12" : type == FS_FAT16 ? "FAT16" : type == FS_EXFAT ? "exFAT" : "FAT32";
}

int usbfs_mount(char *err, size_t errlen)
{
    if (gMounted) return 0;
#if defined(__WIIU__)
    if (!gMochaInit) {
        if (Mocha_InitLibrary() != MOCHA_RESULT_SUCCESS) {
            snprintf(err, errlen, "USB drives need Aroma's Mocha, which isn't running.");
            return -1;
        }
        gMochaInit = true;
    }
    const int ndev = 2;
#else
    const char *const kUsbDevices[] = {"image"};
    const int ndev = 1;
#endif
    if (!gBounce) gBounce = alloc_aligned((size_t)BOUNCE_SECTORS * SECTOR);
    if (!gBounce) {
        snprintf(err, errlen, "Out of memory.");
        return -1;
    }
    char other[96] = "";
    bool sawDrive = false;
    for (int i = 0; i < ndev; i++) {
        if (!raw_open(kUsbDevices[i])) continue;
        sawDrive = true;
        /* mounting only reads the drive: anything that isn't FAT32 is left alone */
        FRESULT fr = f_mount(&gFs, "", 1);
        if (fr == FR_OK && gFs.fs_type == FS_FAT32) {
#if defined(__WIIU__)
            if (AddDevice(usbfs_devoptab()) < 0) {
                f_unmount("");
                raw_close();
                snprintf(err, errlen, "Couldn't set up usb:/.");
                return -1;
            }
#endif
            gMounted = true;
            return 0;
        }
        if (fr == FR_OK) snprintf(other, sizeof(other), "%s", fs_name(gFs.fs_type));
        f_unmount("");
        raw_close();
    }
    if (other[0])
        snprintf(err, errlen, "The USB drive is %s. WiiDrop only installs to FAT32 drives (what USB Loader GX and "
                              "Nintendont use).", other);
    else if (sawDrive)
        snprintf(err, errlen, "No FAT32 file system on the USB drive (a Wii U-formatted drive can't be used for Wii "
                              "games).");
    else
        snprintf(err, errlen, "No USB drive found. Plug a FAT32 drive into the Wii U and try again.");
    return -1;
}

void usbfs_unmount(void)
{
    if (!gMounted) return;
#if defined(__WIIU__)
    RemoveDevice("usb:");
#endif
    f_unmount("");
    raw_close();
    gMounted = false;
}

int usbfs_mounted(void)
{
    return gMounted;
}
