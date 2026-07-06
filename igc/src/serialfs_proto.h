/*
 * serialfs_proto.h - igc <-> igcfs serial protocol extension
 *
 * Command bytes for the file-manager operations the vetted FTX protocol does
 * not cover. They live in the free part of the FTX command space (0x10-0x1F;
 * 0x1F is FTX_CMD_QUIT), so they ride the same packet stream as LIST/START.
 *
 * MUST stay in sync with tools/serialfs/igcfs.py.
 */

#ifndef SERIALFS_PROTO_H
#define SERIALFS_PROTO_H

#define SFS_CMD_DELETE  0x1A   /* request: [cmd][path_len][path]              */
#define SFS_CMD_MKDIR   0x1B   /* request: [cmd][path_len][path]              */
#define SFS_CMD_RENAME  0x1C   /* request: [cmd][old_len][old][new_len][new]  */
#define SFS_CMD_OK      0x1D   /* reply:   [cmd][status]   (status 0 = ok)    */
/* Failures are reported with the stock FTX_CMD_ERROR (0x17) packet.          */

/* Uploads (START direction VICTOR_TO_PC) additionally get a reply after the
 * client's END: SFS_CMD_OK once the server has CRC-verified and atomically
 * stored the file, or FTX_CMD_ERROR if the store failed. Both sides must be
 * updated together - an igc newer than igcfs times out waiting for this
 * reply and reports every upload as failed. */

#endif /* SERIALFS_PROTO_H */
