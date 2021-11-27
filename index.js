module.exports = (Plugin, Library) => {
    const nativeCodeHex = 'PLACEHOLDER';
    const { Patcher, WebpackModules } = Library;

    class Tuxphones extends Plugin {
        writeNativeCode() {
            const nativePath = require('path').join(BdApi.Plugins.folder, 'tuxphones.node');
            require('fs').writeFileSync(nativePath, Buffer.from(nativeCodeHex, 'hex'));
            this.nativeCode = require(nativePath);
            BdApi.showToast('Native code loaded!', {type: 'success'});
        }

        onStart() {
            // Make sure loading fails on incpomatible OSes
            if (process.platform !== 'linux') {
                BdApi.showToast('Incompatible OS.', {type: 'error'});
                throw `Incompatible OS: Current platform: ${process.platform}, linux required`;
            }

            this.writeNativeCode();

            this.nativeCode.onStart(null);

            this.goLiveModal = WebpackModules.find(mod => mod.default?.displayName === "GoLiveModal");

            Patcher.after(this.goLiveModal, "default", (_, [arg], ret) => {
                
            });
        }

        getSettingsPanel() {

        }

        onStop() {
            this.nativeCode.onStop();
        }
    }

    return Tuxphones;
}