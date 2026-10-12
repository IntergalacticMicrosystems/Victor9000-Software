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

/*---------------------------------------------------------------------------
 * Disk images (F6 More): one floppy track per exchange. Multi-byte fields are
 * little-endian. Track = linear index 0..159 (0..79 side 0, 80..159 side 1).
 *
 *  Byte Name       Dir    Payload
 *  0x30 IMG_OPEN   V->PC  [cmd][mode][total_sectors u16][name_len][name]
 *                         mode 0 = create image (Victor reads floppy, pushes
 *                         tracks; name = bare 8.3 name, stored in the dir last
 *                         listed); mode 1 = read image (Victor pulls tracks,
 *                         writes floppy; name = server-relative path).
 *                         total_sectors (1224 or 2391) is meaningful for mode 0.
 *  0x31 IMG_INFO   PC->V  [cmd][status][total_sectors u16]   reply to OPEN
 *                         status 0 ok, 1 not found, 2 not a Victor disk image,
 *                         3 other. Mode 1: total_sectors = the image's size.
 *  0x32 IMG_GET    V->PC  [cmd][track]   mode 1; the server answers with the
 *                         track's IMG_TRACK fragments back to back, or
 *                         FTX_CMD_ERROR.
 *  0x33 IMG_TRACK  both   [cmd][track][frag][nfrag][flags][len u16][data]
 *                         nfrag = ceil(track_bytes / 1024); fragment i covers
 *                         track bytes i*1024 .. min((i+1)*1024, track_bytes).
 *                         flags bit 0 = data RLE-compressed (only when that
 *                         saved bytes). Max 1031 bytes < FTX_MAX_PAYLOAD.
 *                         Mode 0: after a track's fragments the Victor waits
 *                         for SFS_CMD_OK (or FTX_CMD_ERROR) before the next.
 *  0x34 IMG_CLOSE  V->PC  [cmd][status][crc32 u32]
 *                         status 0 = complete, crc32 = CRC-32 of all raw track
 *                         bytes (mode 0: server verifies, then writes the
 *                         .IMG/.RLE; mode 1: server compares with its own);
 *                         status 1 = aborted (server discards). Reply
 *                         SFS_CMD_OK or FTX_CMD_ERROR.
 *
 * An igcfs older than these commands ignores them; igc then times out
 * waiting for IMG_INFO and reports "Server does not support disk images".
 *---------------------------------------------------------------------------*/
#define SFS_CMD_IMG_OPEN    0x30
#define SFS_CMD_IMG_INFO    0x31
#define SFS_CMD_IMG_GET     0x32
#define SFS_CMD_IMG_TRACK   0x33
#define SFS_CMD_IMG_CLOSE   0x34

#define SFS_IMG_FRAG        1024    /* track bytes per IMG_TRACK fragment */
#define SFS_IMG_HDR         7       /* IMG_TRACK header bytes before data */

#endif /* SERIALFS_PROTO_H */
