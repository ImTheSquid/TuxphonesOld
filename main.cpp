// Contains the native code required to access and encode application audio

// PulseAudio to record applications
#include <pulse/thread-mainloop.h>
#include <pulse/error.h>
#include <pulse/stream.h>
#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/def.h>

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

// Forward decls
void PulseStop();

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

std::string RandString(const int len) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    std::string tmp_s;
    tmp_s.reserve(len);

    for (int i = 0; i < len; ++i) {
        tmp_s += alphanum[rand() % (sizeof(alphanum) - 1)];
    }
    
    return tmp_s;
}

void PulseRefreshSinks() {
    foundSinks.clear();
    pa_operation *op = pa_context_get_sink_info_list(paContext, GetSinkInfoCallback, nullptr);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(paMainLoop);
    }
    pa_operation_unref(op);
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
        op = pa_context_load_module(paContext, "module-null-sink", "sink_name=TuxphonesPassthrough", LoadModuleIndexCallback, static_cast<void*>(&moduleLoadType));

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
            (std::string("sink_name=TuxphonesPassthroughCombined sink_properties=slaves=TuxphonesPassthrough,") + passthroughSink).c_str(), 
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

void PulseStop() {
    // Unlock, stop, and free
    if (paContext) {
        pa_context_disconnect(paContext);
        pa_context_unref(paContext);
    }
    pa_threaded_mainloop_stop(paMainLoop);
    // pa_threaded_mainloop_free(paMainLoop);
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

Napi::Value OnStart(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() != 1) {
        throw Napi::Error::New(env, "Wrong number of arguments");
    }

    if (!(info[0].IsString() || info[0].IsNull())) {
        throw Napi::Error::New(env, "First argument must be either String or null");
    }

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
    return exports;
}

NODE_API_MODULE(hello, Init)

} // namespace tuxphones