/**
 * @file ftx_protocol.h
 * @brief File Transfer Protocol Constants and Structures
 *
 * Defines the protocol used for file transfers between PC and Victor 9000.
 * Built on top of the packet protocol layer (packet.h).
 */

#ifndef FTX_PROTOCOL_H
#define FTX_PROTOCOL_H

#include <stdint.h>

/*===========================================================================
 * Protocol Constants
 *===========================================================================*/

/* Command types (first byte of packet payload) */
#define FTX_CMD_START       0x10    /* Start file transfer (metadata) */
#define FTX_CMD_DATA        0x11    /* File data chunk */
#define FTX_CMD_END         0x12    /* End of file (verification) */
#define FTX_CMD_ABORT       0x13    /* Abort transfer */
#define FTX_CMD_STATUS      0x14    /* Status/progress query */
#define FTX_CMD_LIST        0x15    /* Directory listing request */
#define FTX_CMD_LIST_RESP   0x16    /* Directory listing response */
#define FTX_CMD_ERROR       0x17    /* Error message */
#define FTX_CMD_READY       0x18    /* Ready to receive */
#define FTX_CMD_RESEND      0x19    /* Request resend of chunk */
#define FTX_CMD_QUERY       0x1A    /* Query a file's size/existence (PC->Victor) */
#define FTX_CMD_HAVE        0x1B    /* Query response: exists + current size (Victor->PC) */
#define FTX_CMD_QUIT        0x1F    /* Ask a running file server to exit */

/* Test protocol commands (0x20-0x2F reserved for testing) */
#define FTX_CMD_TEST_CMD    0x20    /* Test command from PC */
#define FTX_CMD_TEST_RESULT 0x21    /* Test result from Victor */
#define FTX_CMD_TEST_PING   0x22    /* Ping/heartbeat */
#define FTX_CMD_TEST_PONG   0x23    /* Ping response */

/* Test command sub-types (param1 in FTX_CMD_TEST_CMD) */
#define FTX_TEST_SEND_FILE      0x01    /* Victor sends file to PC */
#define FTX_TEST_RECV_FILE      0x02    /* Victor receives file from PC */
#define FTX_TEST_SET_COMPRESS   0x03    /* Set compression mode */
#define FTX_TEST_SET_BAUD       0x04    /* Set baud rate */
#define FTX_TEST_DELETE_FILE    0x05    /* Delete test file */
#define FTX_TEST_GET_RESULT     0x06    /* Get last transfer result */
#define FTX_TEST_QUIT           0xFF    /* Exit test server */

/* Test result codes */
#define FTX_TEST_OK             0x00    /* Success */
#define FTX_TEST_FAIL           0x01    /* Generic failure */
#define FTX_TEST_FILE_ERROR     0x02    /* File operation error */
#define FTX_TEST_TIMEOUT        0x03    /* Operation timed out */

/* Transfer directions */
#define FTX_DIR_PC_TO_VICTOR    0   /* PC sending to Victor */
#define FTX_DIR_VICTOR_TO_PC    1   /* Victor sending to PC */

/* Compression modes */
#define FTX_COMP_NONE       0       /* No compression */
#define FTX_COMP_RLE        1       /* RLE compression */

/* Flags for START packet */
#define FTX_FLAG_OVERWRITE  0x01    /* Overwrite if file exists */
#define FTX_FLAG_CREATE_DIR 0x02    /* Create directories if needed */
#define FTX_FLAG_PARTIAL    0x04    /* This START transfers only a segment of the
                                       file starting at `offset` (host segmentation
                                       for large/resumable transfers). offset==0
                                       and no flag => whole-file transfer. */
#define FTX_FLAG_FLOWCTRL   0x08    /* The DATA sender requests per-chunk credit
                                       flow control: after writing each chunk the
                                       receiver sends a READY credit, and the
                                       sender holds the next chunk until it
                                       arrives. This keeps the sender off the wire
                                       while the receiver does slow between-chunk
                                       work (e.g. a multi-second floppy write) that
                                       would otherwise overrun the 3-byte RX FIFO.
                                       Negotiated: the receiver confirms support by
                                       setting FTX_CAP_FLOWCTRL in its START READY;
                                       an old peer that ignores the flag simply
                                       never credits and the sender falls back. */

/* Capability bits, reported in the `caps` byte of the READY that answers a
 * START. Lets the DATA sender learn which optional features the receiver
 * actually honored before relying on them. */
#define FTX_CAP_FLOWCTRL    0x01    /* Receiver will send per-chunk credits */

/* Size limits */
#define FTX_MAX_FILENAME    64      /* Maximum path length */
#define FTX_CHUNK_SIZE      1024    /* Max data per chunk */
#define FTX_MAX_PAYLOAD     1040    /* Max packet payload: chunk + DATA header,
                                       rounded up. Must stay <= PKT_MAX_PAYLOAD
                                       (2000, from packet.h). */

/* Timeout and retry settings */
#define FTX_TIMEOUT_MS      5000    /* Timeout per operation (ms) */
#define FTX_MAX_RETRIES     5       /* Maximum chunk retries */

/* Status codes for END packet */
#define FTX_STATUS_OK       0       /* Transfer successful */
#define FTX_STATUS_ERROR    1       /* Transfer failed */
#define FTX_STATUS_ABORTED  2       /* Transfer aborted by user */

/*===========================================================================
 * Error Codes
 *===========================================================================*/

#define FTX_OK              0       /* Success */
#define FTX_ERR_TIMEOUT     -1      /* Timeout waiting for response */
#define FTX_ERR_CRC         -2      /* CRC mismatch (file level) */
#define FTX_ERR_PROTOCOL    -3      /* Protocol error */
#define FTX_ERR_FILE        -4      /* File I/O error */
#define FTX_ERR_DISK_FULL   -5      /* Disk full */
#define FTX_ERR_ABORT       -6      /* Transfer aborted */
#define FTX_ERR_COMPRESS    -7      /* Compression/decompression error */
#define FTX_ERR_MEMORY      -8      /* Out of memory */
#define FTX_ERR_NOT_FOUND   -9      /* File not found */
#define FTX_ERR_EXISTS      -10     /* File already exists */
#define FTX_ERR_PACKET      -11     /* Packet layer error */

/*===========================================================================
 * Packet Structures
 *===========================================================================*/

/**
 * FTX_CMD_START - File metadata packet
 * Sent at start of transfer to describe the file.
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_START (0x10) */
    uint8_t  direction;         /* FTX_DIR_* */
    uint8_t  compression;       /* FTX_COMP_* */
    uint8_t  flags;             /* FTX_FLAG_* */
    uint32_t file_size;         /* Bytes in THIS transfer (segment len; whole file
                                   when FTX_FLAG_PARTIAL is clear and offset==0) */
    uint32_t offset;            /* File position this transfer starts at. 0 for a
                                   whole-file transfer; >0 for a segment (was the
                                   unused compressed_size field - compression is
                                   per-chunk, so this slot was always 0). */
    uint16_t file_date;         /* DOS date format */
    uint16_t file_time;         /* DOS time format */
    uint8_t  file_attr;         /* DOS file attributes */
    uint8_t  name_len;          /* Length of filename (excluding null) */
    char     filename[FTX_MAX_FILENAME]; /* Filename (null-terminated) */
    uint32_t file_crc;          /* CRC-32 of original file contents */
} ftx_start_t;

/**
 * FTX_CMD_DATA - File data chunk packet
 * Carries a chunk of file data.
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_DATA (0x11) */
    uint8_t  flags;             /* Chunk flags: bit 0 = this chunk is compressed */
    uint16_t chunk_num;         /* Chunk sequence number (0-based) */
    uint16_t chunk_size;        /* Bytes in this chunk (max FTX_CHUNK_SIZE) */
    uint8_t  data[FTX_CHUNK_SIZE]; /* Chunk data */
} ftx_data_t;

/* DATA packet flags */
#define FTX_DATA_FLAG_COMPRESSED  0x01  /* This chunk's data is compressed */

/**
 * FTX_CMD_END - End of transfer packet
 * Sent after last data chunk for verification.
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_END (0x12) */
    uint16_t total_chunks;      /* Total number of chunks sent */
    uint32_t bytes_sent;        /* Total bytes sent (may differ from file_size if compressed) */
    uint32_t file_crc;          /* CRC-32 of original file for verification */
    uint8_t  status;            /* FTX_STATUS_* */
} ftx_end_t;

/**
 * FTX_CMD_ABORT - Abort transfer packet
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_ABORT (0x13) */
    uint8_t  reason;            /* Error code */
    char     message[64];       /* Optional error message */
} ftx_abort_t;

/**
 * FTX_CMD_READY - Ready to receive packet
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_READY (0x18) */
    uint8_t  status;            /* 0 = ready, non-zero = error */
    uint8_t  caps;              /* Capability bits (FTX_CAP_*). On the READY that
                                   answers a START, tells the sender which
                                   negotiated features are active. A pre-caps peer
                                   sends a 2-byte READY; readers must treat a
                                   missing caps byte as 0. */
} ftx_ready_t;

/**
 * FTX_CMD_RESEND - Request chunk resend
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_RESEND (0x19) */
    uint16_t chunk_num;         /* Chunk number to resend */
} ftx_resend_t;

/**
 * FTX_CMD_LIST - Directory listing request
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_LIST (0x15) */
    uint8_t  path_len;          /* Length of path */
    char     path[FTX_MAX_FILENAME]; /* Directory path */
} ftx_list_t;

/**
 * FTX_CMD_QUERY - Ask the Victor for a file's current size/existence.
 * Used by host segmentation: before a getfile to learn the total size, and to
 * find the resume point of a partially-transferred file.
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_QUERY (0x1A) */
    uint8_t  name_len;          /* Length of filename */
    char     filename[FTX_MAX_FILENAME]; /* File to query */
} ftx_query_t;

/**
 * FTX_CMD_HAVE - Response to FTX_CMD_QUERY.
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_HAVE (0x1B) */
    uint8_t  exists;            /* 1 if the file exists, 0 otherwise */
    uint32_t size;              /* Current size in bytes (0 if it doesn't exist) */
} ftx_have_t;

/**
 * Directory entry in listing response
 */
typedef struct {
    char     name[13];          /* Filename (8.3 format + null) */
    uint8_t  attr;              /* DOS attributes */
    uint16_t date;              /* DOS date */
    uint16_t time;              /* DOS time */
    uint32_t size;              /* File size */
} ftx_dir_entry_t;

/**
 * FTX_CMD_LIST_RESP - Directory listing response
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_LIST_RESP (0x16) */
    uint8_t  entry_count;       /* Number of entries in this packet */
    uint8_t  more_entries;      /* Non-zero if more entries follow */
    ftx_dir_entry_t entries[1]; /* Variable-length array of entries */
} ftx_list_resp_t;

/**
 * FTX_CMD_ERROR - Error message packet
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_ERROR (0x17) */
    int8_t   error_code;        /* FTX_ERR_* */
    char     message[64];       /* Error message */
} ftx_error_t;

/*===========================================================================
 * Test Protocol Structures
 *===========================================================================*/

/**
 * FTX_CMD_TEST_CMD - Test command packet from PC to Victor
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_TEST_CMD (0x20) */
    uint8_t  test_cmd;          /* Test sub-command (FTX_TEST_*) */
    uint8_t  param1;            /* First parameter (compression, baud index, etc) */
    uint8_t  param2;            /* Second parameter (reserved) */
    char     filename[FTX_MAX_FILENAME]; /* Filename if applicable */
} ftx_test_cmd_t;

/**
 * FTX_CMD_TEST_RESULT - Test result packet from Victor to PC
 */
typedef struct {
    uint8_t  cmd;               /* FTX_CMD_TEST_RESULT (0x21) */
    uint8_t  result_code;       /* FTX_TEST_OK, FTX_TEST_FAIL, etc. */
    uint8_t  error_code;        /* FTX_ERR_* if failure */
    uint8_t  reserved;
    uint32_t bytes_transferred; /* Bytes transferred (for file ops) */
    uint32_t elapsed_ticks;     /* Time taken in DOS ticks (18.2/sec) */
    uint16_t retries;           /* Retry count */
    uint16_t errors;            /* Error count */
    char     message[32];       /* Status message */
} ftx_test_result_t;

/*===========================================================================
 * Utility Macros
 *===========================================================================*/

/* Calculate number of chunks needed for a file */
#define FTX_CHUNKS_NEEDED(size) \
    (((size) + FTX_CHUNK_SIZE - 1) / FTX_CHUNK_SIZE)

/* Maximum entries that fit in a LIST_RESP packet */
#define FTX_MAX_DIR_ENTRIES \
    ((FTX_MAX_PAYLOAD - 3) / sizeof(ftx_dir_entry_t))

#endif /* FTX_PROTOCOL_H */
