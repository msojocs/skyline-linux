/*
 * Shared-memory frame protocol between the frame producer (skyline.node's
 * window backend, skyline_host) and its readers (the DevTools pageframe via
 * sharedMemory.node, tests).  docs/contract.md section 4 describes the layout.
 *
 * File:    /dev/shm/skyline_<key>.dat, i.e. the POSIX shared-memory object
 *          "/skyline_<key>.dat" (Wine uses the same /dev/shm path).
 * Header:  SKYLINE_SHM_HEADER_BYTES: u64 data size | i32 version | padding.
 * Data:    u32 frame id | u32 frame size | RGBA8 pixels.  Writers publish the
 *          size before the id; a reader that sees a new id sees its size.
 */
#ifndef SKYLINE_FRAME_PROTOCOL_H_
#define SKYLINE_FRAME_PROTOCOL_H_

#define SKYLINE_SHM_DIR "/dev/shm"
#define SKYLINE_SHM_NAME_PREFIX "skyline_"
#define SKYLINE_SHM_NAME_SUFFIX ".dat"

enum {
  /* Outer header in front of the data area. */
  SKYLINE_SHM_HEADER_BYTES = 16,
  SKYLINE_SHM_VERSION = 1,
  /* Frame header at the start of the data area. */
  SKYLINE_FRAME_HEADER_BYTES = 8,
  /* The official glue sizes the data area for this many RGBA frames (plus
     the frame header). */
  SKYLINE_FRAME_BUDGET = 4,
};

#endif /* SKYLINE_FRAME_PROTOCOL_H_ */
