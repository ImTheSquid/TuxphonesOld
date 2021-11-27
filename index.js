module.exports = (Plugin, Library) => {
    const nativeCodeHex = 'PLACEHOLDER';
    const { Logger } = Library;

    class Tuxphones extends Plugin {
        writeNativeCode() {
            const nativePath = require('path').join(BdApi.Plugins.folder, 'tuxphones.node');
            require('fs').writeFileSync(nativePath, Buffer.from(nativeCodeHex, 'hex'));
            this.nativeCode = require(nativePath);
            BdApi.showToast('Native code loaded!', {type: 'success'});
        }

        onStart() {
            if (process.platform !== 'linux') {
                BdApi.showToast('Incompatible OS.', {type: 'error'});
                throw `Incompatible OS: Current platform: ${process.platform}, linux required`;
            }

            this.writeNativeCode();

            this.nativeCode.onStart(null);
        }

        getSettingsPanel() {

        }

        onStop() {
            this.nativeCode.onStop();
        }
    }

    return Tuxphones;
}