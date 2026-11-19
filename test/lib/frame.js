'use strict';

// Reference implementation of the pageframe frame-consumer algorithm from
// `js/extensions/skyline/index.js`.  Kept deliberately small so the same code
// can validate frames produced by Windows and Linux producers.

const FRAME_HEADER_BYTES = 8;
const MAX_COPY_RETRIES = 3;

function estimateSharedMemoryBytes(width, height, dpr) {
  return FRAME_HEADER_BYTES + Math.ceil(width * dpr) * Math.ceil(height * dpr) * 4;
}

function frameBytes(width, height, dpr) {
  return Math.ceil(width * dpr) * Math.ceil(height * dpr) * 4;
}

function parseFrame(buffer, offset = 0) {
  const view = Buffer.isBuffer(buffer)
    ? buffer
    : Buffer.from(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  const frameId = view.readUInt32LE(offset);
  const frameSize = view.readUInt32LE(offset + 4);
  const pixelsOffset = offset + FRAME_HEADER_BYTES;
  if (pixelsOffset + frameSize > view.byteLength) {
    throw new RangeError(`frame size ${frameSize} exceeds buffer ${view.byteLength - pixelsOffset}`);
  }
  return {
    frameId,
    frameSize,
    pixels: view.subarray(pixelsOffset, pixelsOffset + frameSize),
  };
}

class FrameReader {
  constructor(shared, key, options = {}) {
    this.shared = shared;
    this.key = key;
    this.width = options.width;
    this.height = options.height;
    this.dpr = options.dpr || 1;
    this.target = Buffer.alloc(options.capacity || estimateSharedMemoryBytes(this.width, this.height, this.dpr));
    this.lastFrameId = 0;
    this.lastFrameSize = 0;
    this.lastFrame = null;
  }

  // Returns null when the shared memory cannot be opened, otherwise
  // { frameId, frameSize, pixels, changed }.
  read() {
    let bytes = null;
    for (let attempt = 0; attempt < MAX_COPY_RETRIES; attempt++) {
      try {
        bytes = this.shared.copyMemoryToBuffer(this.key, this.target);
        break;
      } catch (error) {
        if (error && Number.isInteger(error.requiredSize) && attempt + 1 < MAX_COPY_RETRIES) {
          this.target = Buffer.alloc(error.requiredSize);
          continue;
        }
        throw error;
      }
    }
    if (bytes === null || bytes === undefined) return null;
    if (bytes < FRAME_HEADER_BYTES) {
      throw new RangeError(`copied ${bytes} bytes, shorter than the frame header`);
    }
    const frame = parseFrame(this.target.subarray(0, bytes));
    const changed = frame.frameId !== this.lastFrameId || frame.frameSize !== this.lastFrameSize;
    if (changed) {
      this.lastFrameId = frame.frameId;
      this.lastFrameSize = frame.frameSize;
      this.lastFrame = frame;
    }
    return { ...frame, changed };
  }
}

module.exports = {
  FRAME_HEADER_BYTES,
  MAX_COPY_RETRIES,
  estimateSharedMemoryBytes,
  frameBytes,
  parseFrame,
  FrameReader,
};
