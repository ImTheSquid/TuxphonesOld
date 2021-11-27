var module = require('./build/Debug/tuxphones.node');
module.onStart(null);
console.log(module.getAudioApplications());
module.onStop();