module.exports = (Plugin, Library) => {
    const nativeCodeHex = 'PLACEHOLDER';
    const { Logger, Patcher, WebpackModules, DiscordModules, DiscordClasses } = Library;
    const { React, Dispatcher } = DiscordModules;

    class FormItemOverride extends React.Component {
        constructor(props) {
            super(props);
            this.onSelectionChange = this.onSelectionChange.bind(this);
            this.options = [];

            for (let i = 0; i < props.apps.length; i++) {
                this.options.push({value: i, label: props.apps[i].name, app: props.apps[i]});
            }

            this.state = {
                value: 1
            };

            this.formItem = WebpackModules.find(mod => mod.default?.displayName === 'FormItem');
            this.singleSelect = WebpackModules.find(mod => mod.SingleSelect?.displayName === 'SingleSelect');
        }

        onSelectionChange(val) {
            this.setState({value: val});
        }

        render() {
            const single = this.singleSelect.SingleSelect({onChange: this.onSelectionChange, options: this.options, value: this.state.value});
            return this.formItem.default({title: 'Audio Source', children: single, className: 'modalPadding'});
        }
    }

    class Tuxphones extends Plugin {
        writeNativeCode() {
            const nativePath = require('path').join(BdApi.Plugins.folder, 'tuxphones.node');
            require('fs').writeFileSync(nativePath, Buffer.from(nativeCodeHex, 'hex'));
            this.nativeCode = require(nativePath);
            BdApi.showToast('Native code loaded!', {type: 'success'});
        }

        // Write native code before start to allow for starting and stopping the plugin without crashing BD
        load() {
            // Make sure loading fails on incpomatible OSes
            if (process.platform !== 'linux') {
                BdApi.showToast('Incompatible OS.', {type: 'error'});
                throw `Incompatible OS: Current platform: ${process.platform}, linux required`;
            }

            // this.writeNativeCode();
        }

        onStart() {
            // this.nativeCode.onStart(null);

            BdApi.injectCSS('tuxphones', '.modalPadding { padding: 8px 16px; }')

            this.goLiveModal = WebpackModules.find(mod => mod.default?.displayName === 'Confirm');
            
            this.streamStart = e => {
                Logger.log("STREAM START")
                Logger.log(e)
            };

            Dispatcher.subscribe('STREAM_START', this.streamStart);

            this.streamStop = e => {
                Logger.log("STREAM STOP")
                Logger.log(e)

                // this.nativeCode.stopCapturingApplicationAudio();
            };

            Dispatcher.subscribe('STREAM_STOP', this.streamStop);

            this.streamStatsUpdate = e => {
                Logger.log('STREAM STATS UPDATE')
                Logger.log(e)
            };

            Dispatcher.subscribe('STREAM_STATS_UPDATE', this.streamStatsUpdate);

            Patcher.after(this.goLiveModal, 'default', (_, [arg], ret) => {
                if (!Array.isArray(ret.props.children)) {
                    return;
                }

                const audioApps = [] // this.nativeCode.getAudioApplications();
                if (audioApps.length > 0) {
                    ret.props.children[1] = React.createElement(FormItemOverride, {apps: audioApps});
                } else {
                    ret.props.children[1].props.text = 'Tuxphones couldn\'t detect any audio applications.';
                }
            });
        }

        getSettingsPanel() {

        }

        onStop() {
            Patcher.unpatchAll();
            BdApi.clearCSS('tuxphones');

            Dispatcher.unsubscribe('STREAM_START', this.streamStart);
            Dispatcher.unsubscribe('STREAM_STOP', this.streamStop);
            Dispatcher.unsubscribe('STREAM_STATS_UPDATE', this.streamStatsUpdate);

            // this.nativeCode.onStop();
        }
    }

    return Tuxphones;
}