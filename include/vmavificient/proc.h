/**
 * @file proc.h
 * @brief Process spawning and recursive-deletion helpers for vmavificient.
 */

#ifndef VMAVIFICIENT_PROC_H
#define VMAVIFICIENT_PROC_H

#include <stdarg.h>
#include <stddef.h>

/** @brief Fixed-capacity argv builder for vmav_run. */
typedef struct {
  char buf[16384]; /* argument string storage */
  size_t pos;
  char *argv[256]; /* NULL-terminated argv */
  int argc;
  int overflow; /* set when an arg didn't fit — check before vmav_run */
} VmavCommand;

/** @brief Run @p argv[0] via posix_spawnp, wait, and return the exit code (or -1 on error). */
int vmav_run(char *const argv[]);

/** @brief Recursively remove the directory tree at @p path via nftw (synchronous). */
int vmav_rmtree(const char *path);

/** @brief Recursively remove @p path in a double-forked grandchild (non-blocking). */
int vmav_rmtree_async(const char *path);

/** @brief Initialize a VmavCommand to its empty state. */
void vmav_cmd_init(VmavCommand *c);

/** @brief Append the literal string @p arg to the command's argv. */
void vmav_cmd_arg(VmavCommand *c, const char *arg);

/** @brief Append a printf-formatted argument to the command's argv. */
void vmav_cmd_argf(VmavCommand *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* VMAVIFICIENT_PROC_H */
