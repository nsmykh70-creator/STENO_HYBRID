// POSIX mmap/pwrite shim for mingw-w64: lets native.cpp compile unmodified on Windows.
// Semantics used by native.cpp: mmap with offset 0 always, MAP_PRIVATE read mappings of
// inputs, MAP_SHARED read/write mappings of the decode output, ftruncate to pre-size the
// output, pwrite for the non-mappable-output fallback, madvise is a no-op.
// Limitation: off_t stays 32-bit under mingw, so archives above 4 GiB are not supported
// on Windows builds (all container fields remain 64-bit).
#ifndef STENO_WIN_MMAN_H
#define STENO_WIN_MMAN_H
#ifndef _WIN32
#error "windows-only shim"
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <stdlib.h>

#ifndef PROT_READ
#define PROT_READ 1
#define PROT_WRITE 2
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)
#endif
#define MADV_SEQUENTIAL 0

#ifndef _SSIZE_T_DEFINED
typedef long long ssize_t;
#define _SSIZE_T_DEFINED
#endif

// force binary mode for all CRT open() calls (no O_BINARY at the call sites);
// mingw maps _fmode to (*__p__fmode()), so set it at runtime in a static initializer
static struct StenoFmodeInit{
 StenoFmodeInit(){ _fmode = _O_BINARY; }
} steno_fmode_init_instance;

static void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off){
 (void)addr; (void)flags;
 if(off != 0) return MAP_FAILED;
 if(len == 0) return NULL;
 HANDLE h = (HANDLE)_get_osfhandle(fd);
 if(h == INVALID_HANDLE_VALUE) return MAP_FAILED;
 DWORD fl = (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
 DWORD pa = (prot & PROT_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
 HANDLE m = CreateFileMappingA(h, NULL, fl, 0, 0, NULL);
 if(!m) return MAP_FAILED;
 void *p = MapViewOfFile(m, pa, 0, 0, len);
 CloseHandle(m); /* the mapping object lives while the view exists */
 return p;
}
static int munmap(void *p, size_t n){ (void)n; if(p && p != MAP_FAILED) UnmapViewOfFile(p); return 0; }
static int madvise(void *p, size_t n, int advice){ (void)p; (void)n; (void)advice; return 0; }
static int ftruncate(int fd, long long len){
 HANDLE h = (HANDLE)_get_osfhandle(fd);
 if(h == INVALID_HANDLE_VALUE) return -1;
 LARGE_INTEGER li; li.QuadPart = len;
 if(!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return -1;
 if(!SetEndOfFile(h)) return -1;
 return 0;
}
static ssize_t pwrite(int fd, const void *buf, size_t n, long long off){
 HANDLE h = (HANDLE)_get_osfhandle(fd);
 if(h == INVALID_HANDLE_VALUE) return -1;
 OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
 ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
 DWORD w = 0;
 if(!WriteFile(h, buf, (DWORD)n, &w, &ov)) return -1;
 return (ssize_t)w;
}
/* native.cpp calls ::mkdir(path, mode); mingw's CRT has _mkdir(path) */
#define mkdir(path, mode) _mkdir(path)
#endif
