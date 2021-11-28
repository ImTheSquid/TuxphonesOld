var module = require('./build/Debug/tuxphones.node');
module.onStart(null);
const apps = module.getAudioApplications();
console.log(apps);
if (apps.length > 0) {
    console.log('Initiating audio system for first application')
    let didStop = false;
    module.startCapturingApplicationAudio(apps[0].pid, 48000, data => {
        console.log(data);

        if (!didStop) {
            didStop = true;
            module.stopCapturingApplicationAudio();
            module.onStop();
        }
    })
} else {
    module.onStop();
}