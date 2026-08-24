#pragma once

#if defined(_MSC_VER)

#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#define close _close
#define access _access
#define ftruncate(fd, length) _chsize_s((fd), (length))
#define getpid _getpid
#define lseek _lseek
#define read _read
#define rmdir _rmdir
#define umask _umask
#define write _write

#ifndef F_OK
#define F_OK 0
#endif

#ifndef W_OK
#define W_OK 2
#endif

#ifndef _MODE_T_DEFINED
typedef int mode_t;
#define _MODE_T_DEFINED
#endif

#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE
#define S_IRGRP 0
#define S_IWGRP 0
#define S_IROTH 0
#define S_IWOTH 0
#endif

#else

#include_next <unistd.h>

#endif
