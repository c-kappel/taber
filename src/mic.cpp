// mic.cpp
//
// Minimal Core Audio (Audio Unit / AUHAL) microphone capture example for macOS.
// Captures N seconds of audio from the system's default input device and
// writes it to out.wav as 16-bit PCM.
//
// Build:
//   clang++ -std=c++17 -framework AudioToolbox -framework CoreAudio -framework CoreFoundation mic.cpp -o mic
// Run:
//   ./mic [seconds]     # defaults to 5 seconds
//
// Note: the first run will trigger a macOS microphone permission prompt for
// whatever process launched this binary (e.g. Terminal.app).

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

struct RecorderState {
    AudioStreamBasicDescription format{};
    std::vector<int16_t> samples;
    std::atomic<size_t> writeIndex{0};
    std::atomic<bool> done{false};
};

static AudioUnit gAudioUnit = nullptr;

static void CheckError(OSStatus err, const char* what) {
    if (err != noErr) {
        fprintf(stderr, "Error: %s (status %d)\n", what, (int)err);
        exit(1);
    }
}

// Runs on the real-time audio thread each time a new buffer of mic input is ready.
static OSStatus InputCallback(void* inRefCon,
                               AudioUnitRenderActionFlags* ioActionFlags,
                               const AudioTimeStamp* inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList* /*ioData*/) {
    RecorderState* state = static_cast<RecorderState*>(inRefCon);

    // Reused scratch buffer to receive the rendered audio (avoids per-call malloc).
    static thread_local std::vector<uint8_t> renderBuffer;
    size_t bytesNeeded = (size_t)inNumberFrames * state->format.mBytesPerFrame;
    if (renderBuffer.size() < bytesNeeded) renderBuffer.resize(bytesNeeded);

    AudioBufferList bufferList;
    bufferList.mNumberBuffers = 1;
    bufferList.mBuffers[0].mNumberChannels = state->format.mChannelsPerFrame;
    bufferList.mBuffers[0].mDataByteSize = (UInt32)bytesNeeded;
    bufferList.mBuffers[0].mData = renderBuffer.data();

    // AUHAL doesn't hand us the audio directly in ioData for input-only units;
    // we have to pull it ourselves with AudioUnitRender.
    OSStatus err = AudioUnitRender(gAudioUnit, ioActionFlags, inTimeStamp,
                                    inBusNumber, inNumberFrames, &bufferList);
    if (err != noErr) return err;

    if (state->done.load(std::memory_order_relaxed)) return noErr;

    const int16_t* src = reinterpret_cast<const int16_t*>(bufferList.mBuffers[0].mData);
    size_t sampleCount = (size_t)inNumberFrames * state->format.mChannelsPerFrame;
    size_t start = state->writeIndex.fetch_add(sampleCount, std::memory_order_relaxed);

    if (start + sampleCount <= state->samples.size()) {
        memcpy(state->samples.data() + start, src, sampleCount * sizeof(int16_t));
    } else {
        state->done.store(true, std::memory_order_relaxed);
    }
    return noErr;
}

static void WriteWavFile(const char* path, const RecorderState& state) {
    uint32_t numSamples = (uint32_t)state.writeIndex.load();
    if (numSamples > state.samples.size()) numSamples = (uint32_t)state.samples.size();

    uint16_t numChannels = (uint16_t)state.format.mChannelsPerFrame;
    uint32_t sampleRate = (uint32_t)state.format.mSampleRate;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t dataSize = numSamples * sizeof(int16_t);
    uint32_t riffSize = 36 + dataSize;

    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }

    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16;
    fwrite(&fmtSize, 4, 1, f);
    uint16_t audioFormat = 1; // PCM
    fwrite(&audioFormat, 2, 1, f);
    fwrite(&numChannels, 2, 1, f);
    fwrite(&sampleRate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bitsPerSample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
    fwrite(state.samples.data(), sizeof(int16_t), numSamples, f);
    fclose(f);
}

int main(int argc, char** argv) {
    double durationSeconds = 5.0;
    if (argc > 1) durationSeconds = atof(argv[1]);

    RecorderState state;

    // 1. Find and instantiate the AUHAL — the Audio Unit that talks to hardware I/O.
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) { fprintf(stderr, "Could not find AUHAL component\n"); return 1; }
    CheckError(AudioComponentInstanceNew(comp, &gAudioUnit), "AudioComponentInstanceNew");

    // 2. AUHAL has two buses: 0 = output/speaker, 1 = input/mic.
    //    Enable the input side, disable the output side (we're capture-only).
    UInt32 enable = 1, disable = 0;
    CheckError(AudioUnitSetProperty(gAudioUnit, kAudioOutputUnitProperty_EnableIO,
                                     kAudioUnitScope_Input, 1, &enable, sizeof(enable)),
               "enable input IO");
    CheckError(AudioUnitSetProperty(gAudioUnit, kAudioOutputUnitProperty_EnableIO,
                                     kAudioUnitScope_Output, 0, &disable, sizeof(disable)),
               "disable output IO");

    // 3. Point the unit at the system default input device.
    AudioObjectPropertyAddress defaultInputAddr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID inputDevice = kAudioObjectUnknown;
    UInt32 size = sizeof(inputDevice);
    CheckError(AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultInputAddr,
                                           0, nullptr, &size, &inputDevice),
               "get default input device");
    if (inputDevice == kAudioObjectUnknown) {
        fprintf(stderr, "No default input device found (check mic permission / that a mic is connected)\n");
        return 1;
    }
    CheckError(AudioUnitSetProperty(gAudioUnit, kAudioOutputUnitProperty_CurrentDevice,
                                     kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice)),
               "set current device");

    // 4. Read the hardware's native format on the input side (element 1, scope Input),
    //    then declare our desired client format (16-bit PCM, same rate/channels) on
    //    the output side of element 1 — that's what our callback will receive.
    AudioStreamBasicDescription hwFormat{};
    size = sizeof(hwFormat);
    CheckError(AudioUnitGetProperty(gAudioUnit, kAudioUnitProperty_StreamFormat,
                                     kAudioUnitScope_Input, 1, &hwFormat, &size),
               "get hw stream format");

    AudioStreamBasicDescription clientFormat{};
    clientFormat.mSampleRate = hwFormat.mSampleRate;
    clientFormat.mFormatID = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    clientFormat.mChannelsPerFrame = hwFormat.mChannelsPerFrame;
    clientFormat.mBitsPerChannel = 16;
    clientFormat.mBytesPerFrame = clientFormat.mChannelsPerFrame * sizeof(int16_t);
    clientFormat.mFramesPerPacket = 1;
    clientFormat.mBytesPerPacket = clientFormat.mBytesPerFrame;

    CheckError(AudioUnitSetProperty(gAudioUnit, kAudioUnitProperty_StreamFormat,
                                     kAudioUnitScope_Output, 1, &clientFormat, sizeof(clientFormat)),
               "set client stream format");

    state.format = clientFormat;
    state.samples.resize((size_t)(clientFormat.mSampleRate * clientFormat.mChannelsPerFrame * durationSeconds) + 4096);

    // 5. Install our render callback on the input bus.
    AURenderCallbackStruct callback{};
    callback.inputProc = InputCallback;
    callback.inputProcRefCon = &state;
    CheckError(AudioUnitSetProperty(gAudioUnit, kAudioOutputUnitProperty_SetInputCallback,
                                     kAudioUnitScope_Global, 1, &callback, sizeof(callback)),
               "set input callback");

    // 6. Initialize and start pulling audio.
    CheckError(AudioUnitInitialize(gAudioUnit), "AudioUnitInitialize");
    CheckError(AudioOutputUnitStart(gAudioUnit), "AudioOutputUnitStart");

    printf("Recording %.1f seconds from default input (%.0f Hz, %d ch)...\n",
           durationSeconds, clientFormat.mSampleRate, clientFormat.mChannelsPerFrame);

    while (!state.done.load()) {
        usleep(50 * 1000);
    }

    AudioOutputUnitStop(gAudioUnit);
    AudioUnitUninitialize(gAudioUnit);
    AudioComponentInstanceDispose(gAudioUnit);

    WriteWavFile("out.wav", state);
    printf("Wrote out.wav\n");
    return 0;
}
