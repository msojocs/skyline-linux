'use strict';

const fs = require('node:fs');
const path = require('node:path');

const skylineCases = require('./skyline');
const sharedCases = require('./shared-memory');

const allCases = [...skylineCases, ...sharedCases];

function byId(id) {
  return allCases.find(item => item.id === id);
}

module.exports = { allCases, skylineCases, sharedCases, byId };
