/*
 * serialfs.h - igc serial file-server backend
 *
 * Lets a panel browse and manipulate files served by igcfs.py on a modern PC
 * over a direct serial cable (Victor COM1 / Serial A). Built on the vetted
 * viclibc polled serial + packet + filetrx layers; this module is the only
 * place in igc that knows about that protocol, so panel/fileops just branch on
 * Panel.backend and call these.
 */

#ifndef SERIALFS_H
#define SERIALFS_H

#include "igc.h"
#include "panel.h"

/* Panel.backend values */
#define PANEL_DOS       0
#define PANEL_SERIAL    1

/* Fixed link configuration (these mirror the viclibc SER_PORT_* / SER_BAUD_*
 * indices so callers need not include the viclibc serial headers).
 *
 * Default is 38400: the link is 3-wire with no hardware flow control, so the
 * Victor's 3-byte receive FIFO can overrun on multi-byte packets unless the
 * receiver keeps up. The viclibc packet layer uses a tight polled-receive fast
 * path that drains the FIFO quickly enough to sustain 38400 without gating. */
#define SERIALFS_PORT_A      0
#define SERIALFS_BAUD_9600   6
#define SERIALFS_BAUD_19200  7
#define SERIALFS_BAUD_38400  8
#define SERIALFS_BAUD_DEFAULT SERIALFS_BAUD_38400

/* Open the single serial session on the given port (SER_PORT_A) at the given
 * baud index (SER_BAUD_*). Sends a RESET hello and returns FALSE if the server
 * does not answer (e.g. igcfs.py not running). */
bool_t serialfs_connect(uint8_t port, uint8_t baud_idx);

/* Drop the local session (does not stop the remote server). */
void serialfs_disconnect(void);

bool_t serialfs_is_connected(void);

/* Fill p->files from the server's listing of p->path. Returns 0 on success. */
int serialfs_read_dir(Panel *p);

/* Download remote_rel (relative to the served root) to a local DOS path. */
int serialfs_get_file(const char *remote_rel, const char *local_path);

/* Upload a local DOS file. The server stores it (under its basename) in the
 * directory igc last listed - i.e. the serial panel's current directory. */
int serialfs_put_file(const char *local_path, const char *remote_rel);

/* Recursively copy a whole directory tree (single or nested) between the server
 * and local disk. remote_rel is server-relative; local_path is a full DOS path.
 * They mkdir the destination root, then walk it depth-first. 0 on success. */
int serialfs_get_tree(const char *remote_rel, const char *local_path);
int serialfs_put_tree(const char *local_path, const char *remote_rel);

/* Recursively delete a remote directory tree (its files, then its subdirs, then
 * the directory itself). Needed when a directory move downloads then removes the
 * server-side source. 0 on success. */
int serialfs_delete_tree(const char *remote_rel);

/* Directory operations on the server (remote relative paths). 0 on success. */
int serialfs_delete(const char *remote_rel);
int serialfs_mkdir(const char *remote_rel);
int serialfs_rename(const char *old_rel, const char *new_rel);

/* Build a server-relative path "DIR\\NAME" (or "NAME" at root) from a serial
 * panel's path and a filename. buf must hold at least MAX_FULL_PATH bytes. */
void serialfs_build_rel(Panel *p, const char *name, char *buf);

#endif /* SERIALFS_H */
