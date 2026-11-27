'use strict';

// Runs the scenario selected by SKYLINE_WIN_SCENARIO (see main.js).
const path = require('path');

require(path.join(__dirname, `${process.env.SKYLINE_WIN_SCENARIO}.js`));
