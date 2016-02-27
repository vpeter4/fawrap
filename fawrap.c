/*
 * fawrap - A preload shared library to limit file access to only
 *          some portion of the file. Usable for creating disk
 *          images and accessing partitions inside them with tools
 *          like mke2fs, tune2fs, e2fsck and populatefs.
 *          Some system functions are overriden in library and
 *          preloaded via the LD_PRELOAD environment variable
 *          when using such programs.
 *
 * Usage:
 *   LD_PRELOAD=./fawrap.so FILE=disk.img,offset,length \
 *     <ordinary command>
 *
 *   export LD_PRELOAD=./fawrap.so FILE=disk.img,44040192,33554944
 *   mke2fs -F -q -t ext4 -m 0 disk.img
 *   e2fsck -n disk.img
 *   populatefs -U -d dir3copy disk.img
 *   unset LD_PRELOAD
 *
 * Copyright (C) 2016 Peter Vicman <peter.vicman(at)gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* doesn't work with _FILE_OFFSET_BITS 64
   because we need both functions */
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#define DEFINE_FUNC(type, name, ...) \
  typedef type (*t_##name) (__VA_ARGS__); \
  static t_##name p_##name = NULL

#define DEFINE_DLSYM(name) \
  p_##name = (t_##name) dlsym(RTLD_NEXT, #name)

DEFINE_FUNC(int, open, const char *path, int flags, ...);
DEFINE_FUNC(int, open64, const char *path, int flags, ...);
DEFINE_FUNC(int, __open64_2, const char *path, int flags, ...);
DEFINE_FUNC(int, close, int fd);
DEFINE_FUNC(off_t, lseek, int fd, off_t offset, int whence);
DEFINE_FUNC(off64_t, lseek64, int fd, off64_t offset, int whence);
DEFINE_FUNC(int, __xstat, int x, const char *path, struct stat *buf);
DEFINE_FUNC(int, __xstat64, int x, const char *path, struct stat64 *buf);
DEFINE_FUNC(int, fstat, int fd, struct stat *buf);
DEFINE_FUNC(int, fstat64, int fd, struct stat64 *buf);
DEFINE_FUNC(int, __fxstat64, int vers, int fd, struct stat64 *buf);
DEFINE_FUNC(int, fallocate, int fd, int mode, off_t offset, off_t len);
DEFINE_FUNC(ssize_t, pread64, int fd, void *buf, size_t count, off64_t offset);
DEFINE_FUNC(ssize_t, pwrite64, int fd, const void *buf, size_t count, off64_t offset);

/* some programs can open same file twice with
   2 different fd's (transform to linked list one day) */
#define MAX_FD 16

static char *target_name = NULL;
static int target_fd[MAX_FD];
static off64_t segment_offset = 0;
static off64_t segment_len = 0;
static FILE *debug_stream;
static char debug_level = 2;

#define LOG_ALL   1   /* write always */
#define LOG_ERR   2   /* errors */
#define LOG_INFO  3   /* only redirected calls */
#define LOG_DBG   4   /* all calls */

void dprint(char level, bool our, const char *fmt, ...) {
static char buf[512];
va_list args;

  /* for dbg all system calls are printed */
  if (debug_level < LOG_DBG) {
    if (! (debug_level == LOG_INFO && our)) {
      if (level > debug_level)
        return;
    }
  }

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  printf("%s\n", buf);

  if (debug_stream != NULL) {
    fprintf(debug_stream, "%s\n", buf);
    fflush(debug_stream);
  }
}

bool check_name(const char *path) {
  if (strcmp(path, target_name) == 0)
    return true;

  return false;
}

bool check_fd(int fd) {
int i;

  for (i = 0; i < MAX_FD; i++) {
    if (fd == target_fd[i])
      return true;
  }

  return false;
}

bool add_fd(int fd) {
int i;

  for (i = 0; i < MAX_FD; i++) {
    if (target_fd[i] == 0) {
      target_fd[i] = fd;
      return false;
    }
  }

  return true;
}

void remove_fd(int fd) {
int i;

  for (i = 0; i < MAX_FD; i++) {
    if (fd == target_fd[i]) {
      target_fd[i] = 0;
      return;
    }
  }

  dprint(LOG_ERR, true, "%s(error line %d)", __FUNCTION__, __LINE__);
  exit(1);
}

/* run when a shared library is unloaded */
__attribute__((destructor)) void fini() {
  if (debug_stream != NULL)
    fclose(debug_stream);
}

/* run when a shared library is loaded */
__attribute__((constructor)) void init() {
char *args;
char *p;
int i;

  for (i = 0; i < MAX_FD; i++)
    target_fd[i] = 0;   /*  init fd array */

  DEFINE_DLSYM(open);
  DEFINE_DLSYM(open64);
  DEFINE_DLSYM(__open64_2);
  DEFINE_DLSYM(close);
  DEFINE_DLSYM(lseek);
  DEFINE_DLSYM(lseek64);
  DEFINE_DLSYM(__xstat);
  DEFINE_DLSYM(__xstat64);
  DEFINE_DLSYM(fstat);
  DEFINE_DLSYM(fstat64);
  DEFINE_DLSYM(__fxstat64);
  DEFINE_DLSYM(fallocate);
  DEFINE_DLSYM(pread64);
  DEFINE_DLSYM(pwrite64);

  args = getenv("FILE");
  if (args == NULL) {
    dprint(LOG_ERR, true, "Error: target file not set!");
    fini();
    exit(1);
  }

  target_name = strtok(args, ",");

  p = strtok(NULL, ",");
  if (p == NULL) {
    dprint(LOG_ERR, true, "%s(error line %d)", __FUNCTION__, __LINE__);
    exit(1);
  } else
    segment_offset = strtoull(p, NULL, 10);

  p = strtok(NULL, ",");
  if (p == NULL) {
    dprint(LOG_ERR, true, "%s(error line %d)", __FUNCTION__, __LINE__);
    exit(1);
  } else
    segment_len = strtoull(p, NULL, 10);

  p = strtok(NULL, ",");
  if (p != NULL) {
    if (strcmp(p, "d") == 0)
      debug_level = LOG_DBG;
    else if (strcmp(p, "i") == 0)
      debug_level = LOG_INFO;
  }

  if (debug_level >= LOG_INFO) {
    debug_stream = fopen("fawrap.log", "w");
    if (debug_stream == NULL) {
      dprint(LOG_ERR, true, "%s(error line %d)", __FUNCTION__, __LINE__);
      exit(1);
    }
  }

  dprint(LOG_INFO, true, "fawrap.so target file: %s", target_name);
  dprint(LOG_INFO, true, "fawrap.so      offset: %llu", segment_offset);
  dprint(LOG_INFO, true, "fawrap.so         len: %llu\n", segment_len);
}

/* open and possibly create a file */
int open(const char *path, int flags, ...) {
va_list arg;
mode_t mode = 0;
int res;

  if (flags & O_CREAT) {
    va_start(arg, flags);
    mode = va_arg(arg, mode_t);
    va_end(arg);
  }

  /* if O_CREAT in flags is not specified
     then mode is ignored */
  res = p_open(path, flags, mode);
  dprint(LOG_DBG, check_name(path), "%s(%s, %d, %d) => %d", \
    __FUNCTION__, path, flags, mode, res);

  if (check_name(path) && add_fd(res)) {
    dprint(LOG_ERR, check_name(path), "%s(error line %d)", __FUNCTION__, __LINE__);
    exit(1);
  }

  return res;
}

/* open and possibly create a file */
int open64(const char *path, int flags, ...) {
va_list arg;
mode_t mode = 0;
int res;

  if (flags & O_CREAT) {
    va_start(arg, flags);
    mode = va_arg(arg, mode_t);
    va_end(arg);
  }

  res = p_open64(path, flags, mode);
  dprint(LOG_DBG, check_name(path), "%s(%s, %d, %d) => %d", \
    __FUNCTION__, path, flags, mode, res);

  if (check_name(path) && add_fd(res)) {
    dprint(LOG_ERR, check_name(path), "%s(error line %d)", __FUNCTION__, __LINE__);
    exit(1);
  }

  return res;
}

/* open and possibly create a file
   from libext2fs.so.2 */
int __open64_2(const char *path, int flags) {
int res;

  res = p___open64_2(path, flags);
  dprint(LOG_DBG, check_name(path), "%s(%s, %d) => %d", \
    __FUNCTION__, path, flags, res);

  if (check_name(path) && add_fd(res)) {
    dprint(LOG_ERR, check_name(path), "%s(error line %d)", __FUNCTION__, __LINE__);
    exit(1);
  }

  return res;
}

/* close a file descriptor */
int close(int fd) {
int res;

  res = p_close(fd);
  dprint(LOG_DBG, check_fd(fd), "%s(%d) => %d", \
    __FUNCTION__, fd, res);

  if (check_fd(fd))
    remove_fd(fd);

  return res;
}

/* reposition read/write file offset */
off_t lseek(int fd, off_t offset, int whence) {
off_t res;
off_t offset_new = offset;

  if (check_fd(fd)) {
    /* SEEK_SET: the offset is set to offset bytes */
    if (whence != SEEK_SET) {
      dprint(LOG_DBG, check_fd(fd), "%s(error line %d)", __FUNCTION__, __LINE__);
      exit(1);
    }

    if (offset_new > segment_len) {
      dprint(LOG_ERR, true, "%s offset out of bounds", __FUNCTION__);
      return EINVAL;
    }

    /* we have to move by segment_offset ... */
    offset_new += segment_offset;
  }

  res = p_lseek(fd, offset_new, whence);

  if (check_fd(fd)) {
    if (res != offset_new) {
      errno = EINVAL;
      res = -1;
    }

    /* ... but actual res is only as much has been requested */
    res -= segment_offset;
  }

  dprint(LOG_DBG, check_fd(fd), "%s(%d, %lu, %d) => %lu", \
    __FUNCTION__, fd, offset, whence, res);
  return res;
}

/* reposition read/write file offset */
off64_t lseek64(int fd, off64_t offset, int whence) {
off64_t res;
off64_t offset_new = offset;

  if (check_fd(fd)) {
    if (whence != SEEK_SET) {
      dprint(LOG_DBG, check_fd(fd), "%s(error line %d)", __FUNCTION__, __LINE__);
      exit(1);
    }

    if (offset_new > segment_len) {
      dprint(LOG_ERR, true, "%s offset out of bounds", __FUNCTION__);
      return EINVAL;
    }

    offset_new += segment_offset;
  }

  res = p_lseek64(fd, offset_new, whence);

  if (check_fd(fd)) {
    if (res != offset_new) {
      errno = EINVAL;
      res = -1;
    }

    res -= segment_offset;
  }

  dprint(LOG_DBG, check_fd(fd), "%s(%d, %llu, %d) => %llu", \
    __FUNCTION__, fd, offset, whence, res);
  return res;
}

/* get file status */
int __xstat(int x, const char *path, struct stat *buf) {
int res;

  res = p___xstat(x, path, buf);
  buf->st_size = segment_len;

  dprint(LOG_DBG, check_name(path), "%s(%s, st_mode=%d, st_size=%ld, ...) => %d", \
    __FUNCTION__, path, buf->st_mode, buf->st_size, res);
  return res;
}

/* get file status */
int __xstat64(int x, const char *path, struct stat64 *buf) {
int res;

  res = p___xstat64(x, path, buf);
  buf->st_size = segment_len;

  dprint(LOG_DBG, check_name(path), "%s(%s, st_mode=%d, st_size=%lld, ...) => %d", \
    __FUNCTION__, path, buf->st_mode, buf->st_size, res);
  return res;
}

/* get file status */
int fstat(int fd, struct stat *buf) {
int res;

  res = p_fstat(fd, buf);
  buf->st_size = (off_t) segment_len;

  dprint(LOG_DBG, check_fd(fd), "%s(%d, st_mode=%d, st_size=%ld, ...) => %d", \
    __FUNCTION__, fd, buf->st_mode, buf->st_size, res);
  return res;
}

/* get file status */
int fstat64(int fd, struct stat64 *buf) {
int res;

  res = p_fstat64(fd, buf);
  buf->st_size = segment_len;

  dprint(LOG_DBG, check_fd(fd), "%s(%d, st_mode=%d, st_size=%lld, ...) => %d", \
    __FUNCTION__, fd, buf->st_mode, buf->st_size, res);
  return res;
}

/* get file status */
int __fxstat64(int vers, int fd, struct stat64 *buf) {
int res;

  res = p___fxstat64(vers, fd, buf);
  buf->st_size = segment_len;

  dprint(LOG_DBG, check_fd(fd), "%s(%d, st_mode=%d, st_size=%lld, ...) => %d", \
    __FUNCTION__, fd, buf->st_mode, buf->st_size, res);
  return res;
}

/* manipulate file space */
int fallocate(int fd, int mode, off_t offset, off_t len) {
off_t res;
off_t offset_new = offset;

  if (check_fd(fd)) {
    if (offset_new > segment_len) {
      dprint(LOG_ERR, true, "%s offset out of bounds", __FUNCTION__);
      return ENOSPC;
    }

    /* we have to move by segment_offset */
    offset_new += segment_offset;
  }

  res = p_fallocate(fd, mode, offset_new, len);

  dprint(LOG_DBG, check_fd(fd), "%s(%d, %d, %ld, %ld) => %ld", \
    __FUNCTION__, fd, mode, offset, len, res);
  return res;
}

/* read from file descriptor at a given offset */
ssize_t pread64(int fd, void *buf, size_t count, off64_t offset) {
ssize_t res;
off64_t offset_new = offset;

  if (check_fd(fd)) {
    if (offset_new > segment_len) {
      dprint(LOG_ERR, true, "%s offset out of bounds", __FUNCTION__);
      return ENOSPC;
    }

    /* we have to move by segment_offset */
    offset_new += segment_offset;
  }

  res = p_pread64(fd, buf, count, offset_new);

  dprint(LOG_DBG, check_fd(fd), "%s(%d, %d, %llu) => %d", \
    __FUNCTION__, fd, count, offset, res);
  return res;
}

/* write to a file descriptor at a given offset */
ssize_t pwrite64(int fd, const void *buf, size_t count, off64_t offset) {
ssize_t res;
off64_t offset_new = offset;

  if (check_fd(fd)) {
    if (offset_new > segment_len) {
      dprint(LOG_ERR, true, "%s offset out of bounds", __FUNCTION__);
      return EINVAL;
    }

    /* we have to move by segment_offset */
    offset_new += segment_offset;
  }

  res = p_pwrite64(fd, buf, count, offset_new);

  dprint(LOG_DBG, check_fd(fd), "%s(%d, %d, %llu) => %d", \
    __FUNCTION__, fd, count, offset, res);
  return res;
}
