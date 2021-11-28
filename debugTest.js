function startCapture(apps, module) {
    return new Promise((resolve, reject) => {
        const res = module.startCapturingApplicationAudio(apps[0].pid, 48000, data => {
            console.log('Received data, ending application')
            console.log(data);
    
            resolve(null);
        });
        if (res) {
            reject(res);
        }
    });
}

async function test() {
    var module = require('./build/Debug/tuxphones.node');
    module.onStart(null);
    const apps = module.getAudioApplications();
    console.log(apps);
    if (apps.length > 0) {
        console.log('Initiating audio system for first application')
        const timer = setTimeout(() => {}, 999999)
        startCapture(apps, module).then(val => {
            console.log('Done');
            module.stopCapturingApplicationAudio();
            module.onStop();
            clearTimeout(timer);
        }).catch(rej => {
            console.log(`ERROR! ${rej}`);
            clearTimeout(timer);
        });
    } else {
        module.onStop();
    }
}

Promise.all([test()]);