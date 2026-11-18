// Steinberg ASIO 2.3 binary ABI, declared locally (no ASIO SDK dependency).
// x64 MSVC layout: IASIO non-IUnknown methods use __cdecl, IUnknown uses __stdcall.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>

#ifndef interface
#define interface struct
#endif

#define ASIO_VERSION 102300

typedef long ASIOBool;
typedef long ASIOError;
typedef double ASIOSampleRate;

// ASIOError values
constexpr ASIOError ASE_OK = 0;
constexpr ASIOError ASE_SUCCESS = 0x3f4847a0L;
constexpr ASIOError ASE_NotPresent = -1000;
constexpr ASIOError ASE_HWMalfunction = -999;
constexpr ASIOError ASE_InvalidParameter = -998;
constexpr ASIOError ASE_InvalidMode = -997;
constexpr ASIOError ASE_SPNotAdvancing = -996;
constexpr ASIOError ASE_NoClock = -995;
constexpr ASIOError ASE_NoMemory = -994;

constexpr ASIOBool ASIOFalse = 0;
constexpr ASIOBool ASIOTrue = 1;

#pragma pack(push, 4)

typedef struct ASIOSamples {
    unsigned long hi;
    unsigned long lo;
} ASIOSamples;

typedef struct ASIOTimeStamp {
    unsigned long hi;
    unsigned long lo;
} ASIOTimeStamp;

typedef long ASIOSampleType;

constexpr ASIOSampleType ASIOSTInt16MSB = 0;
constexpr ASIOSampleType ASIOSTInt24MSB = 1;
constexpr ASIOSampleType ASIOSTInt32MSB = 2;
constexpr ASIOSampleType ASIOSTFloat32MSB = 3;
constexpr ASIOSampleType ASIOSTFloat64MSB = 4;
constexpr ASIOSampleType ASIOSTInt32MSB16 = 8;
constexpr ASIOSampleType ASIOSTInt32MSB18 = 9;
constexpr ASIOSampleType ASIOSTInt32MSB20 = 10;
constexpr ASIOSampleType ASIOSTInt32MSB24 = 11;
constexpr ASIOSampleType ASIOSTInt16LSB = 16;
constexpr ASIOSampleType ASIOSTInt24LSB = 17;
constexpr ASIOSampleType ASIOSTInt32LSB = 18;
constexpr ASIOSampleType ASIOSTFloat32LSB = 19;
constexpr ASIOSampleType ASIOSTFloat64LSB = 20;
constexpr ASIOSampleType ASIOSTInt32LSB16 = 24;
constexpr ASIOSampleType ASIOSTInt32LSB18 = 25;
constexpr ASIOSampleType ASIOSTInt32LSB20 = 26;
constexpr ASIOSampleType ASIOSTInt32LSB24 = 27;

typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
} ASIOBufferInfo;

typedef struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

typedef struct ASIOTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    unsigned long samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
} ASIOTimeInfo;

typedef struct ASIOTimeCode {
    double speed;
    ASIOTimeStamp timeCodeSamples;
    unsigned long flags;
    char future[64];
} ASIOTimeCode;

typedef struct ASIOTime {
    long reserved[4];
    struct ASIOTimeInfo timeInfo;
    struct ASIOTimeCode timeCode;
} ASIOTime;

typedef struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
} ASIOClockSource;

// ASIOTimeInfo::flags
constexpr unsigned long kSystemTimeValid = 1;
constexpr unsigned long kSamplePositionValid = 2;
constexpr unsigned long kSampleRateValid = 4;
constexpr unsigned long kSpeedValid = 8;
constexpr unsigned long kSampleRateChanged = 16;
constexpr unsigned long kClockSourceChanged = 32;

// asioMessage selectors
constexpr long kAsioSelectorSupported = 1;
constexpr long kAsioEngineVersion = 2;
constexpr long kAsioResetRequest = 3;
constexpr long kAsioBufferSizeChange = 4;
constexpr long kAsioResyncRequest = 5;
constexpr long kAsioLatenciesChanged = 6;
constexpr long kAsioSupportsTimeInfo = 7;
constexpr long kAsioSupportsTimeCode = 8;
constexpr long kAsioMMCCommand = 9;
constexpr long kAsioSupportsInputMonitor = 10;
constexpr long kAsioSupportsInputGain = 11;
constexpr long kAsioSupportsInputMeter = 12;
constexpr long kAsioSupportsOutputGain = 13;
constexpr long kAsioSupportsOutputMeter = 14;
constexpr long kAsioOverload = 15;

// ASIOFuture selectors (IASIO::future)
constexpr long kAsioEnableTimeCodeRead = 1;
constexpr long kAsioDisableTimeCodeRead = 2;
constexpr long kAsioSetInputMonitor = 3;
constexpr long kAsioTransport = 4;
constexpr long kAsioSetInputGain = 5;
constexpr long kAsioGetInputMeter = 6;
constexpr long kAsioSetOutputGain = 7;
constexpr long kAsioGetOutputMeter = 8;
constexpr long kAsioCanInputMonitor = 9;
constexpr long kAsioCanTimeInfo = 10;
constexpr long kAsioCanTimeCode = 11;

typedef struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

#pragma pack(pop)

// Exact Steinberg IASIO vtable order: 21 IASIO methods, preceded by IUnknown's
// QueryInterface/AddRef/Release = 24 slots. There must be NO virtual destructor;
// a virtual ~IASIO() injects a deleting destructor and shifts every vtable slot.
interface IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};
