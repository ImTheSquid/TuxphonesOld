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
#include <node.h>

#include <optional>

namespace tuxphones {

using v8::Exception;
using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;

typedef struct {
    std::string name;
    uint32_t index;
} paSinkInfo;

// Event loop for Pulse
pa_threaded_mainloop *paMainLoop;
// Server context
pa_context *paContext;
// Current audio stream
pa_stream *paStream;
// Found audio sinks
std::vector<paSinkInfo> foundSinks;
// Combined sink ID
uint32_t combinedSinkIndex;
// PulseInit check
bool pulseDidInit = false;

void GetSinkInfoCallback(pa_context *context, const pa_sink_info *info, int eol, void *userData) {
    // Copy data to foundSinks
    foundSinks.push_back(paSinkInfo{
        .name = std::string(info->name),
        .index = info->index
    });

    pa_threaded_mainloop_signal(paMainLoop, 0);
}

void LoadModuleIndexCallback(pa_context *context, uint32_t index, void *userData) {
    intptr_t isLoadingCombined = (intptr_t)userData;
    if (isLoadingCombined) {
        combinedSinkIndex = index;
    } else {
        foundSinks.push_back(paSinkInfo{
            .name = "TuxphonesPassthrough",
            .index = index
        });
    }

    pa_threaded_mainloop_signal(paMainLoop, 0);
}

void PulseStop();

// Returns null opt on success, string on error
std::optional<std::string> PulseInit(const std::string& passthroughSink) {
    // Create and start main loop
    paMainLoop = pa_threaded_mainloop_new();

    if (int err = pa_threaded_mainloop_start(paMainLoop)) {
        PulseStop();
        return std::string(pa_strerror(err));
    }

    // Connect to server
    if (int err = pa_context_connect(paContext, nullptr, PA_CONTEXT_NOFLAGS, nullptr)) {
        PulseStop();
        return std::string(pa_strerror(err));
    }

    // Figure out if there is an audio sink with name "TuxphonesPassthrough", if not then create it
    pa_threaded_mainloop_lock(paMainLoop);
    pa_operation *op = pa_context_get_sink_info_list(paContext, GetSinkInfoCallback, nullptr);

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(paMainLoop);
    }
    pa_operation_unref(op);

    // Check for sink
    bool tuxSinkFound = false;
    bool tuxCombinedSinkFound = false;
    bool passthroughSinkFound = false;
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
        return "TP_SINK_NOT_FOUND";
    }

    if (!tuxSinkFound) {
        // Create a null sink to read from later
        op = pa_context_load_module(paContext, "module-null-sink", "sink_name=TuxphonesPassthrough", LoadModuleIndexCallback, (void*)0);

        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(paMainLoop);
        }
        pa_operation_unref(op);

    }

    if (!tuxCombinedSinkFound) {
        // Create a combined sink 
        op = pa_context_load_module(paContext, "module-combined-sink", (std::string("sink_name=TuxphonesPassthroughCombined sink_properties=slaves=TuxphonesPassthrough,") + passthroughSink).c_str(), LoadModuleIndexCallback, (void*)1);

        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(paMainLoop);
        }
        pa_operation_unref(op);
    }

    pa_threaded_mainloop_unlock(paMainLoop);

    return std::nullopt;
}

void PulseBeginCaptureAudio(const FunctionCallbackInfo<Value>& args) {

}

void PulseStop() {
    // Unlock, stop, and free
    pa_threaded_mainloop_unlock(paMainLoop);
    pa_threaded_mainloop_stop(paMainLoop);
    pa_threaded_mainloop_free(paMainLoop);
}

// Uninstalls all tuxphones components
void PulseUninstall() {

}

void BeginCaptureApplicationAudio(const FunctionCallbackInfo<Value>& args) {
    PulseBeginCaptureAudio(args);
}

void EndCaptureApplicationAudio(const FunctionCallbackInfo<Value>& args) {

}

void OnStart(const FunctionCallbackInfo<Value>& args) {
    Isolate *isolate = args.GetIsolate();

    if(!args[0]->IsString()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument 0 is not a String").ToLocalChecked()));
    }

    std::optional<std::string> result = PulseInit(std::string(*String::Utf8Value(isolate, args[0])));
}

void OnStop(const FunctionCallbackInfo<Value>& args) {

}

void Initialize(Local<Object> exports) {
    NODE_SET_METHOD(exports, "onStart", OnStart);
    NODE_SET_METHOD(exports, "onStop", OnStop);
    NODE_SET_METHOD(exports, "beginCaptureApplicationAudio", BeginCaptureApplicationAudio);
    NODE_SET_METHOD(exports, "endCaptureApplicationAudio", EndCaptureApplicationAudio);
}

NODE_MODULE(NODE_GYP_MODULE_NAME, Initialize)

} // namespace tuxphones