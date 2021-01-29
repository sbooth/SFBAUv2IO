/*
 * Copyright (c) 2021 Stephen F. Booth <me@sbooth.org>
 * MIT license
 */

#import "SFBAUv2IO.hpp"

#import <limits>
#import <memory>
#import <new>
#import <stdexcept>

#import <os/log.h>

#import "SFBAudioBufferList.hpp"
#import "SFBAudioStreamBasicDescription.hpp"
#import "SFBAudioTimeStamp.hpp"
#import "SFBAudioUnitRecorder.hpp"
#import "SFBExtAudioFile.hpp"

namespace {
	const size_t kScheduledAudioSliceCount = 16;

	SFBAudioBufferList ReadFileContents(CFURLRef url, const AudioStreamBasicDescription& format)
	{
		SFBExtAudioFile eaf;
		eaf.OpenURL(url);

		eaf.SetClientDataFormat(format);
		auto frameLength = eaf.FrameLength();
		if(frameLength > std::numeric_limits<UInt32>::max())
			throw std::overflow_error("Frame length > std::numeric_limits<UInt32>::max()");

		SFBAudioBufferList abl;
		if(!abl.Allocate(format, static_cast<UInt32>(frameLength)))
			throw std::bad_alloc();

		UInt32 frames = static_cast<UInt32>(frameLength);
		eaf.Read(frames, abl);

		return abl;
	}
}

class SFBScheduledAudioSlice : public ScheduledAudioSlice
{
public:
	SFBScheduledAudioSlice()
	{
		memset(this, 0, sizeof(ScheduledAudioSlice));
		mAvailable = true;
	}

	~SFBScheduledAudioSlice()
	{
		if(mBufferList)
			std::free(mBufferList);
	}

	void Clear()
	{
		if(mBufferList)
			std::free(mBufferList);
		memset(this, 0, sizeof(ScheduledAudioSlice));
	}

	std::atomic_bool mAvailable;
};

SFBAUv2IO::SFBAUv2IO()
: mInputUnit(nullptr), mPlayerUnit(nullptr), mMixerUnit(nullptr), mOutputUnit(nullptr), mFirstInputTime(-1), mFirstOutputTime(-1), mScheduledAudioSlices(nullptr)
{
	AudioObjectPropertyAddress addr = {
		.mSelector = kAudioHardwarePropertyDefaultInputDevice,
		.mScope = kAudioObjectPropertyScopeGlobal,
		.mElement = kAudioObjectPropertyElementMaster
	};

	UInt32 size = sizeof(AudioObjectID);
	AudioObjectID inputDevice;
	auto result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &inputDevice);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	addr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
	size = sizeof(AudioObjectID);
	AudioObjectID outputDevice;
	result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &outputDevice);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	Initialize(inputDevice, outputDevice);
	mScheduledAudioSlices = new SFBScheduledAudioSlice [kScheduledAudioSliceCount];
}

SFBAUv2IO::SFBAUv2IO(AudioObjectID inputDevice, AudioObjectID outputDevice)
: mInputUnit(nullptr), mPlayerUnit(nullptr), mMixerUnit(nullptr), mOutputUnit(nullptr), mFirstInputTime(-1), mFirstOutputTime(-1), mScheduledAudioSlices(nullptr)
{
	Initialize(inputDevice, outputDevice);
	mScheduledAudioSlices = new SFBScheduledAudioSlice [kScheduledAudioSliceCount];
}

SFBAUv2IO::~SFBAUv2IO()
{
	Stop();

	if(mInputUnit) {
		AudioUnitUninitialize(mInputUnit);
		AudioComponentInstanceDispose(mInputUnit);
	}

	if(mPlayerUnit) {
		AudioUnitUninitialize(mPlayerUnit);
		AudioComponentInstanceDispose(mPlayerUnit);
	}

	if(mMixerUnit) {
		AudioUnitUninitialize(mMixerUnit);
		AudioComponentInstanceDispose(mMixerUnit);
	}

	if(mOutputUnit) {
		AudioUnitUninitialize(mOutputUnit);
		AudioComponentInstanceDispose(mOutputUnit);
	}

	delete [] mScheduledAudioSlices;
}

void SFBAUv2IO::Start()
{
	if(IsRunning())
		return;

	if(mInputRecorder)
		mInputRecorder->Start();
	if(mPlayerRecorder)
		mPlayerRecorder->Start();
	if(mOutputRecorder)
		mOutputRecorder->Start();

	auto result = AudioOutputUnitStart(mInputUnit);
	SFBCAException::ThrowIfError(result, "AudioOutputUnitStart");
	result = AudioOutputUnitStart(mOutputUnit);
	SFBCAException::ThrowIfError(result, "AudioOutputUnitStart");
}

void SFBAUv2IO::StartAt(const AudioTimeStamp& timeStamp)
{
	if(IsRunning())
		return;

	AudioOutputUnitStartAtTimeParams startAtTime = {
		.mTimestamp = timeStamp,
		.mFlags = 0
	};

	// For some reason this is causing errors in AudioOutputUnitStart()
	auto result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_StartTime, kAudioUnitScope_Global, 0, &startAtTime, sizeof(startAtTime));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");
	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_StartTime, kAudioUnitScope_Global, 0, &startAtTime, sizeof(startAtTime));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	Start();
}

void SFBAUv2IO::Stop()
{
	if(!IsRunning())
		return;

	auto result = AudioOutputUnitStop(mOutputUnit);
	SFBCAException::ThrowIfError(result, "AudioOutputUnitStop");
	result = AudioOutputUnitStop(mInputUnit);
	SFBCAException::ThrowIfError(result, "AudioOutputUnitStop");
	result = AudioUnitReset(mPlayerUnit, kAudioUnitScope_Global, 0);
	SFBCAException::ThrowIfError(result, "AudioUnitReset");

	if(mInputRecorder)
		mInputRecorder->Stop();
	if(mPlayerRecorder)
		mPlayerRecorder->Stop();
	if(mOutputRecorder)
		mOutputRecorder->Stop();

	mFirstInputTime = -1;
	mFirstOutputTime = -1;
}

bool SFBAUv2IO::IsRunning() const
{
	return InputIsRunning() || OutputIsRunning();
}

bool SFBAUv2IO::OutputIsRunning() const
{
	UInt32 value;
	UInt32 size = sizeof(value);
	auto result = AudioUnitGetProperty(mOutputUnit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0, &value, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");
	return value != 0;
}

bool SFBAUv2IO::InputIsRunning() const
{
	UInt32 value;
	UInt32 size = sizeof(value);
	auto result = AudioUnitGetProperty(mInputUnit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0, &value, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");
	return value != 0;
}

void SFBAUv2IO::Play(CFURLRef url)
{
	PlayAt(url, SFBAudioTimeStamp{});
}

void SFBAUv2IO::PlayAt(CFURLRef url, const AudioTimeStamp& timeStamp)
{
	SFBAudioStreamBasicDescription format;
	UInt32 size = sizeof(format);
	auto result = AudioUnitGetProperty(mPlayerUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");

	auto abl = ReadFileContents(url, format);

	SFBScheduledAudioSlice *slice = nullptr;
	for(auto i = 0; i < kScheduledAudioSliceCount; ++i) {
		if(mScheduledAudioSlices[i].mAvailable) {
			slice = mScheduledAudioSlices + i;
			break;
		}
	}

	if(!slice)
		throw std::runtime_error("No available slices");

	slice->Clear();
	slice->mTimeStamp				= timeStamp;
	slice->mCompletionProc			= ScheduledAudioSliceCompletionProc;
	slice->mCompletionProcUserData	= this;
	slice->mNumberFrames			= abl.FrameLength();
	slice->mBufferList				= abl.RelinquishABL();
	slice->mAvailable 				= false;

	result = AudioUnitSetProperty(mPlayerUnit, kAudioUnitProperty_ScheduleAudioSlice, kAudioUnitScope_Global, 0, slice, sizeof(*slice));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	SFBAudioTimeStamp currentPlayTime;
	size = sizeof(currentPlayTime);
	result = AudioUnitGetProperty(mPlayerUnit, kAudioUnitProperty_CurrentPlayTime, kAudioUnitScope_Global, 0, &currentPlayTime, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");

	if(currentPlayTime.SampleTimeIsValid() && currentPlayTime.mSampleTime == -1) {
		SFBAudioTimeStamp startTime{-1.0};
		result = AudioUnitSetProperty(mPlayerUnit, kAudioUnitProperty_ScheduleStartTimeStamp, kAudioUnitScope_Global, 0, &startTime, sizeof(startTime));
		SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");
	}
}

void SFBAUv2IO::GetInputFormat(AudioStreamBasicDescription& format)
{
	UInt32 size = sizeof(format);
	auto result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");
}

void SFBAUv2IO::GetPlayerFormat(AudioStreamBasicDescription& format)
{
	UInt32 size = sizeof(format);
	auto result = AudioUnitGetProperty(mPlayerUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");
}

void SFBAUv2IO::GetOutputFormat(AudioStreamBasicDescription& format)
{
	UInt32 size = sizeof(format);
	auto result = AudioUnitGetProperty(mOutputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");
}

void SFBAUv2IO::SetInputRecordingURL(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format)
{
	mInputRecorder = std::make_unique<SFBAudioUnitRecorder>(mInputUnit, url, fileType, format, 1);
}

void SFBAUv2IO::SetPlayerRecordingURL(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format)
{
	mPlayerRecorder = std::make_unique<SFBAudioUnitRecorder>(mPlayerUnit, url, fileType, format);
}

void SFBAUv2IO::SetOutputRecordingURL(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format)
{
	mOutputRecorder = std::make_unique<SFBAudioUnitRecorder>(mOutputUnit, url, fileType, format);
}

void SFBAUv2IO::Initialize(AudioObjectID inputDevice, AudioObjectID outputDevice)
{
	CreateInputAU(inputDevice);
	CreateOutputAU(outputDevice);
	CreateMixerAU();
	CreatePlayerAU();
	BuildGraph();
	mThroughLatency = MinimumThroughLatency();
}

void SFBAUv2IO::CreateInputAU(AudioObjectID inputDevice)
{
	if(inputDevice == kAudioObjectUnknown)
		throw std::invalid_argument("inputDevice == kAudioObjectUnknown");

	AudioComponentDescription componentDescription = {
		.componentType 			= kAudioUnitType_Output,
		.componentSubType 		= kAudioUnitSubType_HALOutput,
		.componentManufacturer 	= kAudioUnitManufacturer_Apple,
		.componentFlags 		= kAudioComponentFlag_SandboxSafe,
		.componentFlagsMask 	= 0
	};

	auto component = AudioComponentFindNext(nullptr, &componentDescription);
	if(!component)
		throw std::runtime_error("kAudioUnitSubType_HALOutput missing");

	auto result = AudioComponentInstanceNew(component, &mInputUnit);
	SFBCAException::ThrowIfError(result, "AudioComponentInstanceNew");

	UInt32 enableIO = 1;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	enableIO = 0;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	UInt32 startAtZero = 0;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_StartTimestampsAtZero, kAudioUnitScope_Global, 0, &startAtZero, sizeof(startAtZero));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	AURenderCallbackStruct inputCallback = {
		.inputProc = InputRenderCallback,
		.inputProcRefCon = this
	};

	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &inputCallback, sizeof(inputCallback));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	AudioObjectPropertyAddress theAddress = {
		.mSelector = kAudioDevicePropertyNominalSampleRate,
		.mScope = kAudioObjectPropertyScopeGlobal,
		.mElement = kAudioObjectPropertyElementMaster
	};

	Float64 inputDeviceSampleRate;
	UInt32 size = sizeof(inputDeviceSampleRate);
	result = AudioObjectGetPropertyData(inputDevice, &theAddress, 0, nullptr, &size, &inputDeviceSampleRate);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	SFBAudioStreamBasicDescription inputUnitInputFormat;
	size = sizeof(inputUnitInputFormat);
	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &inputUnitInputFormat, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");
//	CFShow(inputUnitInputFormat.Description("input AU input format:  "));


	SFBAudioStreamBasicDescription inputUnitOutputFormat;
	size = sizeof(inputUnitOutputFormat);
	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputUnitOutputFormat, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");

	assert(inputDeviceSampleRate == inputUnitInputFormat.mSampleRate);
	
//	inputUnitOutputFormat.mSampleRate = inputDeviceSampleRate;
	inputUnitOutputFormat.mSampleRate = inputUnitInputFormat.mSampleRate;
	inputUnitOutputFormat.mChannelsPerFrame = inputUnitInputFormat.mChannelsPerFrame;
	result = AudioUnitSetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputUnitOutputFormat, sizeof(inputUnitOutputFormat));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");
//	CFShow(inputUnitOutputFormat.Description("input AU output format: "));

//	UInt32 maxFramesPerSlice;
//	size = sizeof(maxFramesPerSlice);
//	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFramesPerSlice, &size);
//	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");

	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioUnitGetProperty(mInputUnit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &bufferFrameSize, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");

	if(!mInputBufferList.Allocate(inputUnitOutputFormat, bufferFrameSize))
		throw std::bad_alloc();
	if(!mInputRingBuffer.Allocate(inputUnitOutputFormat, 20 * bufferFrameSize))
		throw std::bad_alloc();

	result = AudioUnitInitialize(mInputUnit);
	SFBCAException::ThrowIfError(result, "AudioUnitInitialize");
}

void SFBAUv2IO::CreateOutputAU(AudioObjectID outputDevice)
{
	if(outputDevice == kAudioObjectUnknown)
		throw std::invalid_argument("outputDevice == kAudioObjectUnknown");

	AudioComponentDescription componentDescription = {
		.componentType 			= kAudioUnitType_Output,
		.componentSubType 		= kAudioUnitSubType_HALOutput,
		.componentManufacturer 	= kAudioUnitManufacturer_Apple,
		.componentFlags 		= kAudioComponentFlag_SandboxSafe,
		.componentFlagsMask 	= 0
	};

	auto component = AudioComponentFindNext(nullptr, &componentDescription);
	if(!component)
		throw std::runtime_error("kAudioUnitSubType_HALOutput missing");

	auto result = AudioComponentInstanceNew(component, &mOutputUnit);
	SFBCAException::ThrowIfError(result, "AudioComponentInstanceNew");

	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputDevice, sizeof(outputDevice));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	UInt32 startAtZero = 0;
	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_StartTimestampsAtZero, kAudioUnitScope_Global, 0, &startAtZero, sizeof(startAtZero));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	AURenderCallbackStruct outputCallback = {
		.inputProc = OutputRenderCallback,
		.inputProcRefCon = this
	};

	result = AudioUnitSetProperty(mOutputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &outputCallback, sizeof(outputCallback));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	result = AudioUnitInitialize(mOutputUnit);
	SFBCAException::ThrowIfError(result, "AudioUnitInitialize");
}

void SFBAUv2IO::CreateMixerAU()
{
	AudioComponentDescription componentDescription = {
		.componentType 			= kAudioUnitType_Mixer,
		.componentSubType 		= kAudioUnitSubType_MultiChannelMixer,
		.componentManufacturer 	= kAudioUnitManufacturer_Apple,
		.componentFlags 		= kAudioComponentFlag_SandboxSafe,
		.componentFlagsMask 	= 0
	};

	auto component = AudioComponentFindNext(nullptr, &componentDescription);
	if(!component)
		throw std::runtime_error("kAudioUnitSubType_MultiChannelMixer missing");

	auto result = AudioComponentInstanceNew(component, &mMixerUnit);
	SFBCAException::ThrowIfError(result, "AudioComponentInstanceNew");
}

void SFBAUv2IO::CreatePlayerAU()
{
	AudioComponentDescription componentDescription = {
		.componentType 			= kAudioUnitType_Generator,
		.componentSubType 		= kAudioUnitSubType_ScheduledSoundPlayer,
		.componentManufacturer 	= kAudioUnitManufacturer_Apple,
		.componentFlags 		= kAudioComponentFlag_SandboxSafe,
		.componentFlagsMask 	= 0
	};

	auto component = AudioComponentFindNext(nullptr, &componentDescription);
	if(!component)
		throw std::runtime_error("kAudioUnitSubType_ScheduledSoundPlayer missing");

	auto result = AudioComponentInstanceNew(component, &mPlayerUnit);
	SFBCAException::ThrowIfError(result, "AudioComponentInstanceNew");
}

void SFBAUv2IO::BuildGraph()
{
	// player out -> mixer input 0
	AudioUnitConnection conn = {
		.sourceAudioUnit = mPlayerUnit,
		.sourceOutputNumber = 0,
		.destInputNumber = 0
	};

	auto result = AudioUnitSetProperty(mMixerUnit, kAudioUnitProperty_MakeConnection, kAudioUnitScope_Input, 0, &conn, sizeof(conn));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	// input out -> mixer in 1 [via callback, not connection]
	AURenderCallbackStruct mixerInputCallback = {
		.inputProc = MixerInputRenderCallback,
		.inputProcRefCon = this
	};

	auto format = mInputRingBuffer.Format();
	result = AudioUnitSetProperty(mMixerUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &format, sizeof(format));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	result = AudioUnitSetProperty(mMixerUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 1, &mixerInputCallback, sizeof(mixerInputCallback));
	SFBCAException::ThrowIfError(result, "AudioUnitSetProperty");

	result = AudioUnitInitialize(mMixerUnit);
	SFBCAException::ThrowIfError(result, "AudioUnitInitialize");

	result = AudioUnitInitialize(mPlayerUnit);
	SFBCAException::ThrowIfError(result, "AudioUnitInitialize");

	// Set mixer volumes

	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, 1, 0);
	SFBCAException::ThrowIfError(result, "AudioUnitSetParameter");
	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 1, 1, 0);
	SFBCAException::ThrowIfError(result, "AudioUnitSetParameter");
	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Output, 0, 1, 0);
	SFBCAException::ThrowIfError(result, "AudioUnitSetParameter");

#if DEBUG && 0
	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Pan, kAudioUnitScope_Input, 0, 1, 0);
	SFBCAException::ThrowIfError(result, "AudioUnitSetParameter");
	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Pan, kAudioUnitScope_Input, 1, -1, 0);
	SFBCAException::ThrowIfError(result, "AudioUnitSetParameter");
#endif
}

UInt32 SFBAUv2IO::MinimumInputLatency() const
{
	AudioObjectID inputDevice;
	UInt32 size = sizeof(inputDevice);
	auto result = AudioUnitGetProperty(mInputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDevice, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");

	AudioObjectPropertyAddress propertyAddress = {
		.mSelector 	= kAudioDevicePropertySafetyOffset,
		.mScope 	= kAudioObjectPropertyScopeInput,
		.mElement 	= kAudioObjectPropertyElementMaster
	};

	UInt32 safetyOffset;
	size = sizeof(safetyOffset);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &safetyOffset);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	propertyAddress.mSelector = kAudioDevicePropertyLatency;
	UInt32 latency;
	size = sizeof(latency);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &latency);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	propertyAddress.mSelector = kAudioDevicePropertyStreams;
	size = 0;
	result = AudioObjectGetPropertyDataSize(inputDevice, &propertyAddress, 0, nullptr, &size);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyDataSize");

	auto streamCount = size / sizeof(AudioObjectID);
	std::vector<AudioObjectID> streams(streamCount);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &streams[0]);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	for(auto stream : streams) {
		AudioObjectPropertyAddress propertyAddress = {
			.mSelector 	= kAudioStreamPropertyLatency,
			.mScope 	= kAudioObjectPropertyScopeGlobal,
			.mElement 	= kAudioObjectPropertyElementMaster
		};
		UInt32 streamLatency;
		size = sizeof(streamLatency);
		result = AudioObjectGetPropertyData(stream, &propertyAddress, 0, nullptr, &size, &streamLatency);
		SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "Stream 0x%x latency = %d", stream, streamLatency);
#endif
	}

	propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;
	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &bufferFrameSize);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyDataSize");

#if DEBUG
	os_log_debug(OS_LOG_DEFAULT, "Minimum input latency = %d (%d safety offset + %d buffer size) [device latency = %d]", safetyOffset + bufferFrameSize, safetyOffset, bufferFrameSize, latency);
#endif

	return safetyOffset + bufferFrameSize;
}

UInt32 SFBAUv2IO::MinimumOutputLatency() const
{
	AudioObjectID outputDevice;
	UInt32 size = sizeof(outputDevice);
	auto result = AudioUnitGetProperty(mOutputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputDevice, &size);
	SFBCAException::ThrowIfError(result, "AudioUnitGetProperty");

	AudioObjectPropertyAddress propertyAddress = {
		.mSelector 	= kAudioDevicePropertySafetyOffset,
		.mScope 	= kAudioObjectPropertyScopeOutput,
		.mElement 	= kAudioObjectPropertyElementMaster
	};

	UInt32 safetyOffset;
	size = sizeof(safetyOffset);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &safetyOffset);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	propertyAddress.mSelector = kAudioDevicePropertyLatency;
	UInt32 latency;
	size = sizeof(latency);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &latency);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	propertyAddress.mSelector = kAudioDevicePropertyStreams;
	size = 0;
	result = AudioObjectGetPropertyDataSize(outputDevice, &propertyAddress, 0, nullptr, &size);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyDataSize");

	auto streamCount = size / sizeof(AudioObjectID);
	std::vector<AudioObjectID> streams(streamCount);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &streams[0]);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

	for(auto stream : streams) {
		AudioObjectPropertyAddress propertyAddress = {
			.mSelector 	= kAudioStreamPropertyLatency,
			.mScope 	= kAudioObjectPropertyScopeGlobal,
			.mElement 	= kAudioObjectPropertyElementMaster
		};
		UInt32 streamLatency;
		size = sizeof(streamLatency);
		result = AudioObjectGetPropertyData(stream, &propertyAddress, 0, nullptr, &size, &streamLatency);
		SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "Stream 0x%x latency = %d", stream, streamLatency);
#endif
	}

	propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;
	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &bufferFrameSize);
	SFBCAException::ThrowIfError(result, "AudioObjectGetPropertyData");

#if DEBUG
	os_log_debug(OS_LOG_DEFAULT, "Minimum output latency = %d (%d safety offset + %d buffer size) [device latency = %d]", safetyOffset + bufferFrameSize, safetyOffset, bufferFrameSize, latency);
#endif

	return safetyOffset + bufferFrameSize;
}

OSStatus SFBAUv2IO::InputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(inRefCon);

	if(THIS->mFirstInputTime < 0)
		THIS->mFirstInputTime = inTimeStamp->mSampleTime;

	THIS->mInputBufferList.Reset();
	auto result = AudioUnitRender(THIS->mInputUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, THIS->mInputBufferList);
	SFBCAException::ThrowIfError(result, "AudioUnitRender");

	if(!THIS->mInputRingBuffer.Write(THIS->mInputBufferList, inNumberFrames, inTimeStamp->mSampleTime))
		os_log_debug(OS_LOG_DEFAULT, "SFBCARingBuffer::Write failed at sample time %.0f", inTimeStamp->mSampleTime);

	return noErr;
}

OSStatus SFBAUv2IO::OutputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(inRefCon);

	// Input not yet running
	if(THIS->mFirstInputTime < 0) {
		*ioActionFlags = kAudioUnitRenderAction_OutputIsSilence;
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex)
			memset(static_cast<int8_t *>(ioData->mBuffers[bufferIndex].mData), 0, ioData->mBuffers[bufferIndex].mDataByteSize);
		return noErr;
	}

	if(THIS->mFirstOutputTime < 0) {
		THIS->mFirstOutputTime = inTimeStamp->mSampleTime;
		auto delta = THIS->mFirstInputTime - THIS->mFirstOutputTime;

#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "input → output sample Δ = %.0f\n", delta);
#endif

		THIS->mThroughLatency -= delta;

#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "adjusted latency = %.0f\n", THIS->mThroughLatency);
#endif

		*ioActionFlags = kAudioUnitRenderAction_OutputIsSilence;
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex)
			memset(static_cast<int8_t *>(ioData->mBuffers[bufferIndex].mData), 0, ioData->mBuffers[bufferIndex].mDataByteSize);
		return noErr;
	}

	auto result = AudioUnitRender(THIS->mMixerUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
	SFBCAException::ThrowIfError(result, "AudioUnitRender");

	return noErr;
}

OSStatus SFBAUv2IO::MixerInputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(inRefCon);

	auto adjustedTimeStamp = inTimeStamp->mSampleTime - THIS->mThroughLatency;
	if(!THIS->mInputRingBuffer.Read(ioData, inNumberFrames, adjustedTimeStamp)) {
		os_log_debug(OS_LOG_DEFAULT, "SFBCARingBuffer::Read failed at sample time %.0f", adjustedTimeStamp);
		*ioActionFlags = kAudioUnitRenderAction_OutputIsSilence;
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex)
		memset(static_cast<int8_t *>(ioData->mBuffers[bufferIndex].mData), 0, ioData->mBuffers[bufferIndex].mDataByteSize);
		int64_t startTime, endTime;
		if(THIS->mInputRingBuffer.GetTimeBounds(startTime, endTime))
			THIS->mThroughLatency = inTimeStamp->mSampleTime - startTime;
	}

	return noErr;
}

void SFBAUv2IO::ScheduledAudioSliceCompletionProc(void *userData, ScheduledAudioSlice *slice)
{
//	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(userData);
	static_cast<SFBScheduledAudioSlice *>(slice)->mAvailable = true;
}
