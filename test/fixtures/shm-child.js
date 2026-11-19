'use strict';

// Creates a shared-memory region, fills it with a deterministic pattern, and
// stays alive so another process can observe the mapping.

const modulePath = process.argv[2];
const key = process.argv[3];
const size = Number(process.argv[4] || 128);
const shared = require(modulePath);
shared.setMemory(key, size);
const view = new Uint8Array(shared.getMemory(key));
view[0] = 0x42;
view[1] = 0x43;
view[size - 1] = 0x7f;
process.stdout.write('ready\n');
setInterval(() => {}, 1000);
