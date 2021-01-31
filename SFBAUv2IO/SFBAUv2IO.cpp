//
// Copyright (c) 2021 Stephen F. Booth <me@sbooth.org>
// MIT license
//

#import "SFBAUv2IO.hpp"

#import <cstring>
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
		std::memset(this, 0, sizeof(ScheduledAudioSlice));
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
		std::memset(this, 0, sizeof(ScheduledAudioSlice));
	}

	std::atomic_bool mAvailable;
};

SFBAUv2IO::SFBAUv2IO()
: mInputUnit(nullptr), mPlayerUnit(nullptr), mMixerUnit(nullptr), mOutputUnit(nullptr), mFirstInputSampleTime(-1), mFirstOutputSampleTime(-1), mScheduledAudioSlices(nullptr)
{
	AudioObjectPropertyAddress addr = {
		.mSelector = kAudioHardwarePropertyDefaultInputDevice,
		.mScope = kAudioObjectPropertyScopeGlobal,
		.mElement = kAudioObjectPropertyElementMaster
	};

	UInt32 size = sizeof(AudioObjectID);
	AudioObjectID inputDevice;
	auto result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &inputDevice);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioHardwarePropertyDefaultInputDevice)");

	addr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
	size = sizeof(AudioObjectID);
	AudioObjectID outputDevice;
	result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &outputDevice);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioHardwarePropertyDefaultOutputDevice)");

	Initialize(inputDevice, outputDevice);
	mScheduledAudioSlices = new SFBScheduledAudioSlice [kScheduledAudioSliceCount];
}

SFBAUv2IO::SFBAUv2IO(AudioObjectID inputDevice, AudioObjectID outputDevice)
: mInputUnit(nullptr), mPlayerUnit(nullptr), mMixerUnit(nullptr), mOutputUnit(nullptr), mFirstInputSampleTime(-1), mFirstOutputSampleTime(-1), mScheduledAudioSlices(nullptr)
{
	Initialize(inputDevice, outputDevice);
	mScheduledAudioSlices = new SFBScheduledAudioSlice [kScheduledAudioSliceCount];
}

SFBAUv2IO::~SFBAUv2IO()
{
	if(mOutputUnit)
		AudioOutputUnitStop(mOutputUnit);
	if(mInputUnit)
		AudioOutputUnitStop(mInputUnit);

	if(mInputRecorder)
		mInputRecorder->Stop();
	if(mPlayerRecorder)
		mPlayerRecorder->Stop();
	if(mOutputRecorder)
		mOutputRecorder->Stop();

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
	SFBThrowIfAudioUnitError(result, "AudioOutputUnitStart (mInputUnit)");
	result = AudioOutputUnitStart(mOutputUnit);
	SFBThrowIfAudioUnitError(result, "AudioOutputUnitStart (mOutputUnit)");
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
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_StartTime)");
	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_StartTime, kAudioUnitScope_Global, 0, &startAtTime, sizeof(startAtTime));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_StartTime)");

	Start();
}

void SFBAUv2IO::Stop()
{
	if(!IsRunning())
		return;

	auto result = AudioOutputUnitStop(mInputUnit);
	SFBThrowIfAudioUnitError(result, "AudioOutputUnitStop (mInputUnit)");
	result = AudioOutputUnitStop(mOutputUnit);
	SFBThrowIfAudioUnitError(result, "AudioOutputUnitStop (mOutputUnit)");
	result = AudioUnitReset(mPlayerUnit, kAudioUnitScope_Global, 0);
	SFBThrowIfAudioUnitError(result, "AudioUnitReset (mPlayerUnit)");

	if(mInputRecorder)
		mInputRecorder->Stop();
	if(mPlayerRecorder)
		mPlayerRecorder->Stop();
	if(mOutputRecorder)
		mOutputRecorder->Stop();

	mFirstInputSampleTime = -1;
	mFirstOutputSampleTime = -1;
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
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioOutputUnitProperty_IsRunning)");
	return value != 0;
}

bool SFBAUv2IO::InputIsRunning() const
{
	UInt32 value;
	UInt32 size = sizeof(value);
	auto result = AudioUnitGetProperty(mInputUnit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0, &value, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioOutputUnitProperty_IsRunning)");
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
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioUnitProperty_StreamFormat)");

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
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioUnitProperty_ScheduleAudioSlice)");

	SFBAudioTimeStamp currentPlayTime;
	size = sizeof(currentPlayTime);
	result = AudioUnitGetProperty(mPlayerUnit, kAudioUnitProperty_CurrentPlayTime, kAudioUnitScope_Global, 0, &currentPlayTime, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioUnitProperty_CurrentPlayTime)");

	if(currentPlayTime.SampleTimeIsValid() && currentPlayTime.mSampleTime == -1) {
		SFBAudioTimeStamp startTime{-1.0};
		result = AudioUnitSetProperty(mPlayerUnit, kAudioUnitProperty_ScheduleStartTimeStamp, kAudioUnitScope_Global, 0, &startTime, sizeof(startTime));
		SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioUnitProperty_ScheduleStartTimeStamp)");
	}
}

void SFBAUv2IO::GetInputFormat(AudioStreamBasicDescription& format)
{
	UInt32 size = sizeof(format);
	auto result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioUnitProperty_StreamFormat)");
}

void SFBAUv2IO::GetPlayerFormat(AudioStreamBasicDescription& format)
{
	UInt32 size = sizeof(format);
	auto result = AudioUnitGetProperty(mPlayerUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioUnitProperty_StreamFormat)");
}

void SFBAUv2IO::GetOutputFormat(AudioStreamBasicDescription& format)
{
	UInt32 size = sizeof(format);
	auto result = AudioUnitGetProperty(mOutputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioUnitProperty_StreamFormat)");
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
	SFBThrowIfAudioObjectError(result, "AudioComponentInstanceNew");

	UInt32 enableIO = 1;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_EnableIO)");

	enableIO = 0;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_EnableIO)");

	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_CurrentDevice)");

	UInt32 startAtZero = 0;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_StartTimestampsAtZero, kAudioUnitScope_Global, 0, &startAtZero, sizeof(startAtZero));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_StartTimestampsAtZero)");

	AURenderCallbackStruct inputCallback = {
		.inputProc = InputRenderCallback,
		.inputProcRefCon = this
	};

	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &inputCallback, sizeof(inputCallback));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_SetInputCallback)");

	AudioObjectPropertyAddress theAddress = {
		.mSelector = kAudioDevicePropertyNominalSampleRate,
		.mScope = kAudioObjectPropertyScopeGlobal,
		.mElement = kAudioObjectPropertyElementMaster
	};

	Float64 inputDeviceSampleRate;
	UInt32 size = sizeof(inputDeviceSampleRate);
	result = AudioObjectGetPropertyData(inputDevice, &theAddress, 0, nullptr, &size, &inputDeviceSampleRate);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertyNominalSampleRate)");

	SFBAudioStreamBasicDescription inputUnitInputFormat;
	size = sizeof(inputUnitInputFormat);
	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &inputUnitInputFormat, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioUnitProperty_StreamFormat)");
//	CFShow(inputUnitInputFormat.Description("input AU input format:  "));


	SFBAudioStreamBasicDescription inputUnitOutputFormat;
	size = sizeof(inputUnitOutputFormat);
	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputUnitOutputFormat, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioUnitProperty_StreamFormat)");

	assert(inputDeviceSampleRate == inputUnitInputFormat.mSampleRate);
	
//	inputUnitOutputFormat.mSampleRate = inputDeviceSampleRate;
	inputUnitOutputFormat.mSampleRate = inputUnitInputFormat.mSampleRate;
	inputUnitOutputFormat.mChannelsPerFrame = inputUnitInputFormat.mChannelsPerFrame;
	result = AudioUnitSetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputUnitOutputFormat, sizeof(inputUnitOutputFormat));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioUnitProperty_StreamFormat)");
//	CFShow(inputUnitOutputFormat.Description("input AU output format: "));

//	UInt32 maxFramesPerSlice;
//	size = sizeof(maxFramesPerSlice);
//	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFramesPerSlice, &size);
//	SFBAudioUnitThrowIfError(result, "AudioUnitGetProperty");

	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioUnitGetProperty(mInputUnit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &bufferFrameSize, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioDevicePropertyBufferFrameSize)");

	if(!mInputBufferList.Allocate(inputUnitOutputFormat, bufferFrameSize))
		throw std::bad_alloc();
	if(!mInputRingBuffer.Allocate(inputUnitOutputFormat, 20 * bufferFrameSize))
		throw std::bad_alloc();

	result = AudioUnitInitialize(mInputUnit);
	SFBThrowIfAudioUnitError(result, "AudioUnitInitialize");
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
	SFBThrowIfAudioObjectError(result, "AudioComponentInstanceNew");

	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputDevice, sizeof(outputDevice));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_CurrentDevice)");

	UInt32 startAtZero = 0;
	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_StartTimestampsAtZero, kAudioUnitScope_Global, 0, &startAtZero, sizeof(startAtZero));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioOutputUnitProperty_StartTimestampsAtZero)");

	AURenderCallbackStruct outputCallback = {
		.inputProc = OutputRenderCallback,
		.inputProcRefCon = this
	};

	result = AudioUnitSetProperty(mOutputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &outputCallback, sizeof(outputCallback));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioUnitProperty_SetRenderCallback)");

	result = AudioUnitInitialize(mOutputUnit);
	SFBThrowIfAudioUnitError(result, "AudioUnitInitialize");
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
	SFBThrowIfAudioObjectError(result, "AudioComponentInstanceNew");
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
	SFBThrowIfAudioObjectError(result, "AudioComponentInstanceNew");
}

void SFBAUv2IO::BuildGraph()
{
	// player out -> mixer input 0
	AudioUnitConnection conn = {
		.sourceAudioUnit 		= mPlayerUnit,
		.sourceOutputNumber 	= 0,
		.destInputNumber 		= 0
	};

	auto result = AudioUnitSetProperty(mMixerUnit, kAudioUnitProperty_MakeConnection, kAudioUnitScope_Input, 0, &conn, sizeof(conn));
	SFBThrowIfAudioUnitError(result, "AudioUnitSetProperty (kAudioUnitProperty_MakeConnection)");

	result = AudioUnitInitialize(mMixerUnit);
	SFBThrowIfAudioUnitError(result, "AudioUnitInitialize");

	result = AudioUnitInitialize(mPlayerUnit);
	SFBThrowIfAudioUnitError(result, "AudioUnitInitialize");

	// Set mixer volumes

	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, 1, 0);
	SFBThrowIfAudioUnitError(result, "AudioUnitSetParameter (kMultiChannelMixerParam_Volume)");
	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Output, 0, 1, 0);
	SFBThrowIfAudioUnitError(result, "AudioUnitSetParameter (kMultiChannelMixerParam_Volume)");
}

UInt32 SFBAUv2IO::MinimumInputLatency() const
{
	AudioObjectID inputDevice;
	UInt32 size = sizeof(inputDevice);
	auto result = AudioUnitGetProperty(mInputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDevice, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioOutputUnitProperty_CurrentDevice)");

	AudioObjectPropertyAddress propertyAddress = {
		.mSelector 	= kAudioDevicePropertySafetyOffset,
		.mScope 	= kAudioObjectPropertyScopeInput,
		.mElement 	= kAudioObjectPropertyElementMaster
	};

	UInt32 safetyOffset;
	size = sizeof(safetyOffset);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &safetyOffset);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertySafetyOffset)");

	propertyAddress.mSelector = kAudioDevicePropertyLatency;
	UInt32 latency;
	size = sizeof(latency);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &latency);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertyLatency)");

	propertyAddress.mSelector = kAudioDevicePropertyStreams;
	size = 0;
	result = AudioObjectGetPropertyDataSize(inputDevice, &propertyAddress, 0, nullptr, &size);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyDataSize (kAudioDevicePropertyStreams)");

	auto streamCount = size / sizeof(AudioObjectID);
	std::vector<AudioObjectID> streams(streamCount);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &streams[0]);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertyStreams)");

	for(auto stream : streams) {
		AudioObjectPropertyAddress propertyAddress = {
			.mSelector 	= kAudioStreamPropertyLatency,
			.mScope 	= kAudioObjectPropertyScopeGlobal,
			.mElement 	= kAudioObjectPropertyElementMaster
		};
		UInt32 streamLatency;
		size = sizeof(streamLatency);
		result = AudioObjectGetPropertyData(stream, &propertyAddress, 0, nullptr, &size, &streamLatency);
		SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioStreamPropertyLatency)");

#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "Input stream 0x%x latency %d", stream, streamLatency);
#endif
	}

	propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;
	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &bufferFrameSize);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertyBufferFrameSize)");

#if DEBUG
	os_log_debug(OS_LOG_DEFAULT, "Minimum input latency %d (%d safety offset + %d buffer size) [device latency %d]", safetyOffset + bufferFrameSize, safetyOffset, bufferFrameSize, latency);
#endif

	return safetyOffset + bufferFrameSize;
}

UInt32 SFBAUv2IO::MinimumOutputLatency() const
{
	AudioObjectID outputDevice;
	UInt32 size = sizeof(outputDevice);
	auto result = AudioUnitGetProperty(mOutputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputDevice, &size);
	SFBThrowIfAudioUnitError(result, "AudioUnitGetProperty (kAudioOutputUnitProperty_CurrentDevice)");

	AudioObjectPropertyAddress propertyAddress = {
		.mSelector 	= kAudioDevicePropertySafetyOffset,
		.mScope 	= kAudioObjectPropertyScopeOutput,
		.mElement 	= kAudioObjectPropertyElementMaster
	};

	UInt32 safetyOffset;
	size = sizeof(safetyOffset);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &safetyOffset);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertySafetyOffset)");

	propertyAddress.mSelector = kAudioDevicePropertyLatency;
	UInt32 latency;
	size = sizeof(latency);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &latency);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertyLatency)");

	propertyAddress.mSelector = kAudioDevicePropertyStreams;
	size = 0;
	result = AudioObjectGetPropertyDataSize(outputDevice, &propertyAddress, 0, nullptr, &size);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyDataSize (kAudioDevicePropertyStreams)");

	auto streamCount = size / sizeof(AudioObjectID);
	std::vector<AudioObjectID> streams(streamCount);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &streams[0]);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertyStreams)");

	for(auto stream : streams) {
		AudioObjectPropertyAddress propertyAddress = {
			.mSelector 	= kAudioStreamPropertyLatency,
			.mScope 	= kAudioObjectPropertyScopeGlobal,
			.mElement 	= kAudioObjectPropertyElementMaster
		};
		UInt32 streamLatency;
		size = sizeof(streamLatency);
		result = AudioObjectGetPropertyData(stream, &propertyAddress, 0, nullptr, &size, &streamLatency);
		SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioStreamPropertyLatency)");

#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "Output stream 0x%x latency %d", stream, streamLatency);
#endif
	}

	propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;
	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &bufferFrameSize);
	SFBThrowIfAudioObjectError(result, "AudioObjectGetPropertyData (kAudioDevicePropertyBufferFrameSize)");

#if DEBUG
	os_log_debug(OS_LOG_DEFAULT, "Minimum output latency %d (%d safety offset + %d buffer size) [device latency %d]", safetyOffset + bufferFrameSize, safetyOffset, bufferFrameSize, latency);
#endif

	return safetyOffset + bufferFrameSize;
}

OSStatus SFBAUv2IO::InputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(inRefCon);

	if(THIS->mFirstInputSampleTime < 0)
		THIS->mFirstInputSampleTime = inTimeStamp->mSampleTime;

	THIS->mInputBufferList.Reset();
	auto result = AudioUnitRender(THIS->mInputUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, THIS->mInputBufferList);
//	SFBAudioUnitThrowIfError(result, "AudioUnitRender (mInputUnit)");
	if(result != noErr)
		os_log_error(OS_LOG_DEFAULT, "Error rendering input: %d", result);

	if(!THIS->mInputRingBuffer.Write(THIS->mInputBufferList, inNumberFrames, inTimeStamp->mSampleTime))
		os_log_debug(OS_LOG_DEFAULT, "SFBCARingBuffer::Write failed at sample time %.0f", inTimeStamp->mSampleTime);

	return result;
}

OSStatus SFBAUv2IO::OutputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(inRefCon);

	// Input not yet running
	if(THIS->mFirstInputSampleTime < 0) {
		*ioActionFlags = kAudioUnitRenderAction_OutputIsSilence;
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex)
			std::memset(static_cast<int8_t *>(ioData->mBuffers[bufferIndex].mData), 0, ioData->mBuffers[bufferIndex].mDataByteSize);
		return noErr;
	}

	if(THIS->mFirstOutputSampleTime < 0) {
		THIS->mFirstOutputSampleTime = inTimeStamp->mSampleTime;
		auto delta = THIS->mFirstInputSampleTime - THIS->mFirstOutputSampleTime;

#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "input → output sample Δ %.0f\n", delta);
#endif

		THIS->mThroughLatency -= delta;

#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "Adjusted through latency %.0f\n", THIS->mThroughLatency);
#endif

		*ioActionFlags = kAudioUnitRenderAction_OutputIsSilence;
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex)
			std::memset(static_cast<int8_t *>(ioData->mBuffers[bufferIndex].mData), 0, ioData->mBuffers[bufferIndex].mDataByteSize);
		return noErr;
	}

	auto result = AudioUnitRender(THIS->mMixerUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
//	SFBAudioUnitThrowIfError(result, "AudioUnitRender (mMixerUnit)");
	if(result != noErr)
		os_log_error(OS_LOG_DEFAULT, "Error rendering mixer output: %d", result);

	return result;
}

void SFBAUv2IO::ScheduledAudioSliceCompletionProc(void *userData, ScheduledAudioSlice *slice)
{
//	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(userData);
	static_cast<SFBScheduledAudioSlice *>(slice)->mAvailable = true;
}
