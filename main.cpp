// Contains the native code required to access and encode application audio

// PulseAudio to record applications
#include <pulse/thread-mainloop.h>
#include <pulse/error.h>
#include <pulse/stream.h>
#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/def.h>
#include <pulse/proplist.h>

// Opus to encode audio
#include <opus/opus.h>

// Node to interface with BD
#include <napi.h>

#include <optional>
#include <string>
#include <vector>

namespace tuxphones {

typedef struct {
    std::string name;
    uint32_t index;
} paSinkInfo;

typedef struct {
    std::string name;
    pid_t pid;
    uint32_t index;
    uint32_t sinkIndex;
} applicationInfo;

// Event loop for Pulse
pa_threaded_mainloop *paMainLoop;
// Server context
pa_context *paContext;
pa_context_state paContextState = PA_CONTEXT_UNCONNECTED;
// Current audio stream
pa_stream *paStream;
// Found audio sinks
std::vector<paSinkInfo> foundSinks;
// Tuxphones sink ID
uint32_t tuxphonesSinkIndex;
// Combined sink ID
uint32_t combinedSinkIndex;
// Original application sink, held while capturing to be able to restore functionality
uint32_t sinkInputRestoreIndex;
std::optional<applicationInfo> targetApp;

// Opus encoder
OpusEncoder *encoder;

Napi::FunctionReference encodeCallback;

void GetSinkInfoCallback(pa_context *context, const pa_sink_info *info, int eol, void *userData) {
    // Copy data to foundSinks
    if (info) {
        foundSinks.push_back(paSinkInfo{
            .name = std::string(info->name),
            .index = info->index
        });
    }

    if (eol) {
        pa_threaded_mainloop_signal(paMainLoop, 0);
    }
}

void LoadModuleIndexCallback(pa_context *context, uint32_t index, void *userData) {
    if (*static_cast<int*>(userData)) {
        combinedSinkIndex = index;
    } else {
        tuxphonesSinkIndex = index;
    }

    pa_threaded_mainloop_signal(paMainLoop, 0);
}

void ContextStateDidChange(pa_context *context, void *userData) {
    paContextState = pa_context_get_state(context);

    pa_threaded_mainloop_signal(paMainLoop, 0);
}

void GenericSuccessCallback(pa_context *context, int success, void *userData) {
    pa_threaded_mainloop_signal(paMainLoop, 0);
}

void GetSinkInputInfoCallback(pa_context *context, const pa_sink_input_info *info, int eol, void *userData) {
    auto *infoVec = static_cast<std::vector<applicationInfo>*>(userData);
    if (info) {
        const char *pid = pa_proplist_gets(info->proplist, "application.process.id");
        if (pid) {
            char *str = static_cast<char*>(malloc(strlen(pid) + 1));
            strcpy(str, pid);
            infoVec->push_back(applicationInfo{
                .name = std::string(info->name),
                .pid = atoi(str),
                .index = info->index,
                .sinkIndex = info->sink
            });
            free(str);
        }
    }

    if (eol) {
        pa_threaded_mainloop_signal(paMainLoop, 0);
    }
}

void StreamReadCallback(pa_stream *stream, size_t nbytes, void *userData) {
    const void *data = malloc(sizeof(opus_int16) * nbytes);
    // Read current data then clear it for next session
    pa_stream_peek(stream, &data, &nbytes);
    pa_stream_drop(stream);

    // Send data to Opus for encoding
    uint8_t *encoded = static_cast<uint8_t*>(malloc(sizeof(uint8_t) * (nbytes)));
    int res = opus_encode(encoder, static_cast<const opus_int16*>(data), nbytes, encoded, nbytes);
    if (res < 0) {
        free(encoded);
        return;
    }

    Napi::Uint8Array arr = Napi::Uint8Array();

    for (size_t i = 0; i < nbytes; ++i) {
        arr[i] = encoded[i];
    }
    
    encodeCallback.Value().Call({ arr });
    
    free(encoded);
}

void PulseRefreshSinks() {
    foundSinks.clear();
    pa_operation *op = pa_context_get_sink_info_list(paContext, GetSinkInfoCallback, nullptr);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(paMainLoop);
    }
    pa_operation_unref(op);
}

void PulseStop() {
    // Unlock, stop, and free
    if (paStream) {
        pa_stream_disconnect(paStream);
        pa_stream_unref(paStream);
    }
    if (paContext) {
        pa_context_disconnect(paContext);
        pa_context_unref(paContext);
    }
    pa_threaded_mainloop_stop(paMainLoop);
    // pa_threaded_mainloop_free(paMainLoop);
}

// Sets up audio interfaces for recording application audio
// Returns null opt on success, string on error
std::optional<std::string> PulseSetupAudio(const std::optional<std::string> passthroughOverride) {
    // If override device is given then hook off of that sink, otherwise use default sink
    const std::string passthroughSink = passthroughOverride.value_or("@DEFAULT_SINK@");

    // Check for sink
    bool tuxSinkFound = false;
    bool tuxCombinedSinkFound = false;
    // Only care about finding passthrough sink if not using default
    bool passthroughSinkFound = !passthroughOverride.has_value();
    for (const auto &foundSink: foundSinks) {
        if (foundSink.name == "TuxphonesPassthrough") {
            tuxSinkFound = true;
        } else if (foundSink.name == "TuxphonesPassthroughCombined") {
            tuxCombinedSinkFound = true;
        } else if (foundSink.name == passthroughSink) {
            passthroughSinkFound = true;
        }
    }

    // Sink for audio passthrough not found, error
    if (!passthroughSinkFound) {
        PulseStop();
        return "PASSTHROUGH_SINK_NOT_FOUND";
    }

    pa_threaded_mainloop_lock(paMainLoop);
    pa_operation *op;
    int moduleLoadType;
    if (!tuxSinkFound) {
        // Create a null sink to read from later
        moduleLoadType = 0;
        op = pa_context_load_module(paContext, "module-null-sink", "sink_name=Tuxphones", LoadModuleIndexCallback, static_cast<void*>(&moduleLoadType));

        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(paMainLoop);
        }
        pa_operation_unref(op);

        // Get audio device list now that new set is added
        PulseRefreshSinks();
    }

    if (!tuxCombinedSinkFound) {
        // Create a combined sink 
        moduleLoadType = 1;
        op = pa_context_load_module(paContext, "module-combine-sink", 
            (std::string("sink_name=TuxphonesPassthrough sink_properties=slaves=TuxphonesPassthrough,") + passthroughSink).c_str(), 
            LoadModuleIndexCallback, static_cast<void*>(&moduleLoadType));

        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(paMainLoop);
        }
        pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(paMainLoop);

    return std::nullopt;
}

// Initializes Pulse subsystems
// Returns null opt on success, string on error
std::optional<std::string> PulseInit() {
    // Create and start main loop
    paMainLoop = pa_threaded_mainloop_new();

    if (int err = pa_threaded_mainloop_start(paMainLoop)) {
        PulseStop();
        return "Error starting mainloop: " + std::string(pa_strerror(err));
    }

    // Connect to server
    pa_threaded_mainloop_lock(paMainLoop);
    paContext = pa_context_new(pa_threaded_mainloop_get_api(paMainLoop), "tuxphones");
    pa_context_set_state_callback(paContext, ContextStateDidChange, nullptr);
    if (int err = pa_context_connect(paContext, nullptr, PA_CONTEXT_NOFLAGS, nullptr)) {
        PulseStop();
        return "Error connecting context: " + std::string(pa_strerror(err));
    }

    while(paContextState == PA_CONTEXT_UNCONNECTED || (PA_CONTEXT_IS_GOOD(paContextState) && paContextState != PA_CONTEXT_READY)) {
        pa_threaded_mainloop_wait(paMainLoop);
    }

    if (paContextState != PA_CONTEXT_READY) {
        PulseStop();
        return "Context failed to connect: " + std::to_string(paContextState);
    }

    // Get audio device list
    PulseRefreshSinks();
    pa_threaded_mainloop_unlock(paMainLoop);

    return std::nullopt;
}

// Uninstalls all tuxphones components
void PulseTeardownAudio() {
    pa_threaded_mainloop_lock(paMainLoop);

    pa_operation *op = pa_context_unload_module(paContext, combinedSinkIndex, GenericSuccessCallback, nullptr);

    while(pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(paMainLoop);
    }
    pa_operation_unref(op);

    op = pa_context_unload_module(paContext, tuxphonesSinkIndex, GenericSuccessCallback, nullptr);

    while(pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(paMainLoop);
    }
    pa_operation_unref(op);

    pa_threaded_mainloop_unlock(paMainLoop);
}

std::vector<applicationInfo> PulseGetApplications() {
    pa_threaded_mainloop_lock(paMainLoop);
    std::vector<applicationInfo> infoVec;
    pa_operation *op = pa_context_get_sink_input_info_list(paContext, GetSinkInputInfoCallback, static_cast<void*>(&infoVec));

    while(pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(paMainLoop);
    }
    pa_operation_unref(op);

    pa_threaded_mainloop_unlock(paMainLoop);
    return infoVec;
}

std::optional<std::string> PulseStartCapture(const pid_t applicationPID, const uint32_t bitrate) {
    pa_threaded_mainloop_lock(paMainLoop);

    // Set sink input (application) to combined passthrough sink
    for (const auto& app : PulseGetApplications()) {
        if (app.pid == applicationPID) {
            targetApp = app;
            sinkInputRestoreIndex = app.sinkIndex;
            pa_operation *op = pa_context_move_sink_input_by_index(paContext, app.index, combinedSinkIndex, GenericSuccessCallback, nullptr);

            while(pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
                pa_threaded_mainloop_wait(paMainLoop);
            }
            pa_operation_unref(op);
        }
    }

    // Initialize stream
    /*pa_proplist *formatProps = pa_proplist_new();
    if (int error = pa_proplist_set(formatProps, PA_PROP_FORMAT_CHANNEL_MAP, &paChannelMap, sizeof(pa_channel_map))) {
        PulseStop();
        return std::string(pa_strerror(error));
    }
    if (int error = pa_proplist_sets(formatProps, PA_PROP_FORMAT_SAMPLE_FORMAT, std::to_string(PA_SAMPLE_S16NE).c_str())) {
        PulseStop();
        return std::string(pa_strerror(error));
    }
    if (int error = pa_proplist_sets(formatProps, PA_PROP_FORMAT_RATE, std::to_string(bitrate).c_str())) {
        PulseStop();
        return std::string(pa_strerror(error));
    }

    pa_format_info info {
        .encoding = PA_ENCODING_ANY,
        .plist = formatProps
    };
    pa_format_info * const infoPtr = &info;*/
    pa_sample_spec sampleSpec {
        .format = PA_SAMPLE_S16NE,
        .rate = bitrate,
        .channels = 1
    };

    pa_channel_map channelMap;
    channelMap.channels = 1;
    channelMap.map[0] = PA_CHANNEL_POSITION_MONO;

    paStream = pa_stream_new(paContext, "TuxphonesStream", &sampleSpec, &channelMap);
    // pa_proplist_free(formatProps);

    pa_stream_set_read_callback(paStream, StreamReadCallback, nullptr);

    // Connects stream to sink monitor
    if (int error = pa_stream_set_monitor_stream(paStream, targetApp.value().index)) {
        PulseStop();
        return std::string(pa_strerror(error));
    }
    if (int error = pa_stream_connect_record(paStream, "Tuxphones", nullptr, PA_STREAM_NOFLAGS)) { // might need to change "Tuxphones" to "Tuxphones.monitor"
        PulseStop();
        return std::string(pa_strerror(error));
    }

    pa_threaded_mainloop_unlock(paMainLoop);

    return std::nullopt;
}

void PulseStopCapture() {
    pa_threaded_mainloop_lock(paMainLoop);

    if (targetApp.has_value()) {
        pa_operation *op = pa_context_move_sink_input_by_index(paContext, targetApp.value().index, sinkInputRestoreIndex, GenericSuccessCallback, nullptr);

        while(pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(paMainLoop);
        }
        pa_operation_unref(op);

        targetApp = std::nullopt;
    }

    pa_stream_disconnect(paStream);
    paStream = nullptr;

    pa_threaded_mainloop_unlock(paMainLoop);
}

Napi::Value StartCapturingApplicationAudio(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (encoder || paStream) {
        throw Napi::Error::New(env, "Capture already started. Stop capture before attempting to start again");
    }

    if (info.Length() != 3) {
        throw Napi::Error::New(env, "Wrong number of arguments, expected 3");
    }

    if (!(info[0].IsNumber() && info[1].IsNumber() && info[2].IsFunction())) {
        throw Napi::Error::New(env, "Args: pid: Number, sampleRate: Number, callback: Function(Uint8Array)");
    }

    const uint32_t sampleRate = info[1].ToNumber().Uint32Value();

    // Initialize Opus
    int error;
    encoder = opus_encoder_create(sampleRate, 1, OPUS_APPLICATION_AUDIO, &error);
    if (error) {
        throw Napi::Error::New(env, "Opus Error: " + std::string(opus_strerror(error)));
    }

    encodeCallback = Napi::Persistent(info[2].As<Napi::Function>());
    PulseStartCapture(info[0].ToNumber().Uint32Value(), sampleRate);

    return env.Undefined();
}

Napi::Value StopCapturingApplicationAudio(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    PulseStopCapture();
    opus_encoder_destroy(encoder);
    encoder = nullptr;

    return env.Undefined();
}

// Gets the applications that are currently producing audio
Napi::Value GetAudioApplications(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    std::vector<applicationInfo> infoVec = PulseGetApplications();

    Napi::Array arr = Napi::Array::New(env);

    int i = 0;
    for (const auto& app : infoVec) {
        Napi::Object obj = Napi::Object::New(env);
        obj.Set("name", Napi::String::From(env, app.name));
        obj.Set("pid", Napi::Number::From(env, app.pid));
        arr[i++] = obj;
    }

    return arr;
}

Napi::Value OnStart(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() != 1) {
        throw Napi::Error::New(env, "Wrong number of arguments, expected 1");
    }

    if (!(info[0].IsString() || info[0].IsNull())) {
        throw Napi::Error::New(env, "Args: overrideDevice: String|null");
    }

    // Initialize Pulse
    std::optional<std::string> result = PulseInit();
    if (result.has_value()) {
        throw Napi::Error::New(env, "Error initializing Pulse: " + result.value());
    }

    result = PulseSetupAudio(info[0].IsNull() ? std::nullopt : std::optional<std::string>(info[0].ToString().Utf8Value()));
    if (result.has_value()) {
        throw Napi::Error::New(env, "Error initializing Pulse: " + result.value());
    }

    return env.Undefined();
}

Napi::Value OnStop(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    PulseTeardownAudio();
    PulseStop();

    return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "onStart"),
                Napi::Function::New(env, OnStart));

    exports.Set(Napi::String::New(env, "onStop"),
                Napi::Function::New(env, OnStop));

    exports.Set(Napi::String::New(env, "startCapturingApplicationAudio"),
                Napi::Function::New(env, StartCapturingApplicationAudio));

    exports.Set(Napi::String::New(env, "stopCapturingApplicationAudio"),
                Napi::Function::New(env, StopCapturingApplicationAudio));

    exports.Set(Napi::String::New(env, "getAudioApplications"),
                Napi::Function::New(env, GetAudioApplications));
    return exports;
}

NODE_API_MODULE(hello, Init)

} // namespace tuxphones