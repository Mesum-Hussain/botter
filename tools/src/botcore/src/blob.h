#ifndef BC_BLOB_H
#define BC_BLOB_H

/*
 * Agent pack format ("BOTPACK"), appended to the botcore executable by
 * botter_pack. All integers little-endian. The ELF loader ignores trailing
 * bytes, so the result is still a normal executable.
 *
 *   [botcore ELF][zero padding to a 4096 boundary][BLOB][FOOTER]
 *
 * BLOB (offsets relative to BLOB start):
 *    0  char[8] "BOTBLOB1"
 *    8  u32     count
 *   12  u32     reserved (0)
 *   16  entry[count], 32 bytes each, sorted by path (strcmp, strictly ascending):
 *         u32 path_off   NUL-terminated path string
 *         u32 path_len   (without the NUL)
 *         u64 data_off   data bytes followed by one NUL byte
 *         u64 data_len   (without the NUL)
 *         u32 kind       0 data, 1 exec ELF, 2 exec script
 *         u32 reserved
 *   ... path strings and data
 *
 * FOOTER (the last 32 bytes of the file):
 *    0  u64     blob_off  (file offset of BLOB)
 *    8  u64     blob_len
 *   16  u32     crc32 of BLOB (IEEE 802.3, as zlib)
 *   20  u32     reserved (0)
 *   24  char[8] "BOTPACK1"
 *
 * The writer (botter_pack) is a separate program and carries its own copy of
 * this description. Keep both in sync; bump the magic digits on any change.
 */

#define BLOB_MAGIC        "BOTBLOB1"
#define FOOTER_MAGIC      "BOTPACK1"
#define BLOB_HEADER_SIZE  16
#define BLOB_ENTRY_SIZE   32
#define FOOTER_SIZE       32
#define BLOB_ALIGN        4096

#endif
