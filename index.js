module.exports = (Plugin, Library) => {
    const nativeCodeHex = 'PLACEHOLDER';
    const { Logger } = Library;

    class Tuxphones extends Plugin {
        writeNativeCode() {
            const fs = require('fs');
            const path = require('path');
            const nativePath = path.join(BdApi.Plugins.folder, 'tuxphones.node');
            fs.writeFileSync(nativePath, Buffer.from(nativeCodeHex, 'hex'));
            this.nativeCode = require(nativePath);
            BdApi.showToast('Native code loaded!', {type: 'success'});
        }

        onStart() {
            if (process.platform !== 'linux') {
                BdApi.showToast('Incompatible OS.', {type: 'error'});
                throw `Incompatible OS: Current platform: ${process.platform}, linux required`;
            }

            this.writeNativeCode();
        }

        getSettingsPanel() {

        }

        onStop() {

        }
    }

    return Tuxphones;
}