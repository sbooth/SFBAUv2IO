/*
 * Copyright (c) 2021 Stephen F. Booth <me@sbooth.org>
 * MIT license
 */

#import "SFBAUv2IO.hpp"

#import <memory>

#import <Accelerate/Accelerate.h>

#import "SFBAudioBufferList.hpp"
#import "SFBAudioFormat.hpp"
#import "SFBAudioUnitRecorder.hpp"

template <>
struct ::std::default_delete<OpaqueExtAudioFile> {
	default_delete() = default;
	template <class U>
	constexpr default_delete(default_delete<U>) noexcept {}
	void operator()(OpaqueExtAudioFile *eaf) const noexcept { /* auto result =*/ ExtAudioFileDispose(eaf); }
};

namespace {
	const size_t kScheduledAudioSliceCount = 16;

	SFBAudioBufferList ReadFileContents(CFURLRef url, const AudioStreamBasicDescription& format)
	{
		ExtAudioFileRef eaf;
		auto result = ExtAudioFileOpenURL(url, &eaf);
		assert(result == noErr);

		auto eaf_ptr = std::unique_ptr<OpaqueExtAudioFile>(eaf);

		result = ExtAudioFileSetProperty(eaf, kExtAudioFileProperty_ClientDataFormat, sizeof(format), &format);
		assert(result == noErr);

		SInt64 frameLength;
		UInt size = sizeof(frameLength);
		result = ExtAudioFileGetProperty(eaf, kExtAudioFileProperty_FileLengthFrames, &size, &frameLength);
		assert(result == noErr);

		SFBAudioBufferList abl;
		assert(abl.Allocate(format, static_cast<UInt32>(frameLength)));

		UInt32 frames = static_cast<UInt32>(frameLength);
		result = ExtAudioFileRead(eaf, &frames, abl);
		assert(result == noErr);

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
	assert(result == noErr);

	addr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
	size = sizeof(AudioObjectID);
	AudioObjectID outputDevice;
	result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &outputDevice);
	assert(result == noErr);

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
	assert(result == noErr);
	result = AudioOutputUnitStart(mOutputUnit);
	assert(result == noErr);

	mFirstInputTime = -1;
	mFirstOutputTime = -1;
}

void SFBAUv2IO::StartAt(const AudioTimeStamp& startTime)
{
	if(IsRunning())
		return;

	AudioOutputUnitStartAtTimeParams startAtTime = {
		.mTimestamp = startTime,
		.mFlags = 0
	};

	// For some reason this is causing errors in AudioOutputUnitStart()
	auto result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_StartTime, kAudioUnitScope_Global, 0, &startAtTime, sizeof(startAtTime));
	assert(result == noErr);
	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_StartTime, kAudioUnitScope_Global, 0, &startAtTime, sizeof(startAtTime));
	assert(result == noErr);

	Start();
}

void SFBAUv2IO::Stop()
{
	if(!IsRunning())
		return;

	auto result = AudioOutputUnitStop(mOutputUnit);
	assert(result == noErr);
	result = AudioOutputUnitStop(mInputUnit);
	assert(result == noErr);

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
	assert(result == noErr);
	return value != 0;
}

bool SFBAUv2IO::InputIsRunning() const
{
	UInt32 value;
	UInt32 size = sizeof(value);
	auto result = AudioUnitGetProperty(mInputUnit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0, &value, &size);
	assert(result == noErr);
	return value != 0;
}

void SFBAUv2IO::Play(CFURLRef url)
{
	AudioTimeStamp ts{};
	FillOutAudioTimeStampWithSampleTime(ts, -1);
	PlayAt(url, ts);
}

void SFBAUv2IO::PlayAt(CFURLRef url, const AudioTimeStamp& startTime)
{
	SFBAudioFormat format;
	UInt32 size = sizeof(format);
	auto result = AudioUnitGetProperty(mPlayerUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
	assert(result == noErr);

	auto abl = ReadFileContents(url, format);

	SFBScheduledAudioSlice *slice = nullptr;
	for(auto i = 0; i < kScheduledAudioSliceCount; ++i) {
		if(mScheduledAudioSlices[i].mAvailable) {
			slice = mScheduledAudioSlices + i;
			break;
		}
	}
	assert(slice);

	slice->Clear();
	slice->mTimeStamp				= AudioTimeStamp{};
	slice->mCompletionProc			= ScheduledAudioSliceCompletionProc;
	slice->mCompletionProcUserData	= this;
	slice->mNumberFrames			= abl.FrameLength();
	slice->mBufferList				= abl.RelinquishABL();
	slice->mAvailable 				= false;

	result = AudioUnitSetProperty(mPlayerUnit, kAudioUnitProperty_ScheduleAudioSlice, kAudioUnitScope_Global, 0, slice, sizeof(*slice));
	assert(result == noErr);

	result = AudioUnitSetProperty(mPlayerUnit, kAudioUnitProperty_ScheduleStartTimeStamp, kAudioUnitScope_Global, 0, &startTime, sizeof(startTime));
	assert(result == noErr);
}

void SFBAUv2IO::SetInputRecordingPath(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format)
{
	mInputRecorder = std::make_unique<SFBAudioUnitRecorder>(mInputUnit, url, fileType, format, 1);
}

void SFBAUv2IO::SetPlayerRecordingPath(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format)
{
	mPlayerRecorder = std::make_unique<SFBAudioUnitRecorder>(mPlayerUnit, url, fileType, format);
}

void SFBAUv2IO::SetOutputRecordingPath(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format)
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
	mLatency = MinimumThroughLatency();
}

void SFBAUv2IO::CreateInputAU(AudioObjectID inputDevice)
{
	assert(inputDevice != kAudioObjectUnknown);

	AudioComponentDescription componentDescription = {
		.componentType 			= kAudioUnitType_Output,
		.componentSubType 		= kAudioUnitSubType_HALOutput,
		.componentManufacturer 	= kAudioUnitManufacturer_Apple,
		.componentFlags 		= kAudioComponentFlag_SandboxSafe,
		.componentFlagsMask 	= 0
	};

	auto component = AudioComponentFindNext(nullptr, &componentDescription);
	assert(component);

	auto result = AudioComponentInstanceNew(component, &mInputUnit);
	assert(result == noErr);

	UInt32 enableIO = 1;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
	assert(result == noErr);

	enableIO = 0;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));
	assert(result == noErr);

	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice));
	assert(result == noErr);

	UInt32 startAtZero = 0;
	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_StartTimestampsAtZero, kAudioUnitScope_Global, 0, &startAtZero, sizeof(startAtZero));
	assert(result == noErr);

	AURenderCallbackStruct inputCallback = {
		.inputProc = InputRenderCallback,
		.inputProcRefCon = this
	};

	result = AudioUnitSetProperty(mInputUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &inputCallback, sizeof(inputCallback));
	assert(result == noErr);

	AudioObjectPropertyAddress theAddress = {
		.mSelector = kAudioDevicePropertyNominalSampleRate,
		.mScope = kAudioObjectPropertyScopeGlobal,
		.mElement = kAudioObjectPropertyElementMaster
	};

	Float64 inputDeviceSampleRate;
	UInt32 size = sizeof(inputDeviceSampleRate);
	result = AudioObjectGetPropertyData(inputDevice, &theAddress, 0, nullptr, &size, &inputDeviceSampleRate);
	assert(result == noErr);

	SFBAudioFormat inputUnitInputFormat;
	size = sizeof(inputUnitInputFormat);
	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &inputUnitInputFormat, &size);
	assert(result == noErr);
//	CFShow(inputUnitInputFormat.Description("input AU input format:  "));


	SFBAudioFormat inputUnitOutputFormat;
	size = sizeof(inputUnitOutputFormat);
	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputUnitOutputFormat, &size);
	assert(result == noErr);

	assert(inputDeviceSampleRate == inputUnitInputFormat.mSampleRate);
	
//	inputUnitOutputFormat.mSampleRate = inputDeviceSampleRate;
	inputUnitOutputFormat.mSampleRate = inputUnitInputFormat.mSampleRate;
	inputUnitOutputFormat.mChannelsPerFrame = inputUnitInputFormat.mChannelsPerFrame;
	result = AudioUnitSetProperty(mInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputUnitOutputFormat, sizeof(inputUnitOutputFormat));
	assert(result == noErr);
//	CFShow(inputUnitOutputFormat.Description("input AU output format: "));

//	UInt32 maxFramesPerSlice;
//	size = sizeof(maxFramesPerSlice);
//	result = AudioUnitGetProperty(mInputUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFramesPerSlice, &size);
//	assert(result == noErr);

	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioUnitGetProperty(mInputUnit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &bufferFrameSize, &size);
	assert(result == noErr);

	assert(mInputBufferList.Allocate(inputUnitOutputFormat, bufferFrameSize));
	assert(mInputRingBuffer.Allocate(inputUnitOutputFormat, 20 * bufferFrameSize));

	result = AudioUnitInitialize(mInputUnit);
	assert(result == noErr);
}

void SFBAUv2IO::CreateOutputAU(AudioObjectID outputDevice)
{
	assert(outputDevice != kAudioObjectUnknown);

	AudioComponentDescription componentDescription = {
		.componentType 			= kAudioUnitType_Output,
		.componentSubType 		= kAudioUnitSubType_HALOutput,
		.componentManufacturer 	= kAudioUnitManufacturer_Apple,
		.componentFlags 		= kAudioComponentFlag_SandboxSafe,
		.componentFlagsMask 	= 0
	};

	auto component = AudioComponentFindNext(nullptr, &componentDescription);
	assert(component);

	auto result = AudioComponentInstanceNew(component, &mOutputUnit);
	assert(result == noErr);

	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputDevice, sizeof(outputDevice));
	assert(result == noErr);

	UInt32 startAtZero = 0;
	result = AudioUnitSetProperty(mOutputUnit, kAudioOutputUnitProperty_StartTimestampsAtZero, kAudioUnitScope_Global, 0, &startAtZero, sizeof(startAtZero));
	assert(result == noErr);

	AURenderCallbackStruct outputCallback = {
		.inputProc = OutputRenderCallback,
		.inputProcRefCon = this
	};

	result = AudioUnitSetProperty(mOutputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &outputCallback, sizeof(outputCallback));
	assert(result == noErr);

	result = AudioUnitInitialize(mOutputUnit);
	assert(result == noErr);
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
	assert(component);
	auto result = AudioComponentInstanceNew(component, &mMixerUnit);
	assert(result == noErr);
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
	assert(component);
	auto result = AudioComponentInstanceNew(component, &mPlayerUnit);
	assert(result == noErr);
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
	assert(result == noErr);

	// input out -> mixer in 1 [via callback, not connection]
	AURenderCallbackStruct mixerInputCallback = {
		.inputProc = MixerInputRenderCallback,
		.inputProcRefCon = this
	};

	auto format = mInputRingBuffer.Format();
	result = AudioUnitSetProperty(mMixerUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &format, sizeof(format));
	assert(result == noErr);

	result = AudioUnitSetProperty(mMixerUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 1, &mixerInputCallback, sizeof(mixerInputCallback));
	assert(result == noErr);

	result = AudioUnitInitialize(mMixerUnit);
	assert(result == noErr);

	result = AudioUnitInitialize(mPlayerUnit);
	assert(result == noErr);

	// Set mixer volumes

	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, 1, 0);
	assert(result == noErr);
	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 1, 1, 0);
	assert(result == noErr);
	result = AudioUnitSetParameter(mMixerUnit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Output, 0, 1, 0);
	assert(result == noErr);
}

UInt32 SFBAUv2IO::MinimumInputLatency() const
{
	AudioObjectID inputDevice;
	UInt32 size = sizeof(inputDevice);
	auto result = AudioUnitGetProperty(mInputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDevice, &size);
	assert(result == noErr);

	AudioObjectPropertyAddress propertyAddress = {
		.mSelector 	= kAudioDevicePropertySafetyOffset,
		.mScope 	= kAudioObjectPropertyScopeInput,
		.mElement 	= kAudioObjectPropertyElementMaster
	};

	UInt32 safetyOffset;
	size = sizeof(safetyOffset);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &safetyOffset);
	assert(result == noErr);

	propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;
	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioObjectGetPropertyData(inputDevice, &propertyAddress, 0, nullptr, &size, &bufferFrameSize);
	assert(result == noErr);

	return safetyOffset + bufferFrameSize;
}

UInt32 SFBAUv2IO::MinimumOutputLatency() const
{
	AudioObjectID outputDevice;
	UInt32 size = sizeof(outputDevice);
	auto result = AudioUnitGetProperty(mOutputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputDevice, &size);
	assert(result == noErr);

	AudioObjectPropertyAddress propertyAddress = {
		.mSelector 	= kAudioDevicePropertySafetyOffset,
		.mScope 	= kAudioObjectPropertyScopeOutput,
		.mElement 	= kAudioObjectPropertyElementMaster
	};

	UInt32 safetyOffset;
	size = sizeof(safetyOffset);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &safetyOffset);
	assert(result == noErr);

	propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;
	UInt32 bufferFrameSize;
	size = sizeof(bufferFrameSize);
	result = AudioObjectGetPropertyData(outputDevice, &propertyAddress, 0, nullptr, &size, &bufferFrameSize);
	assert(result == noErr);

	return safetyOffset + bufferFrameSize;
}

OSStatus SFBAUv2IO::InputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(inRefCon);

	if(THIS->mFirstInputTime < 0)
		THIS->mFirstInputTime = inTimeStamp->mSampleTime;

	THIS->mInputBufferList.Reset();
	auto result = AudioUnitRender(THIS->mInputUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, THIS->mInputBufferList);
	assert(result == noErr);

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
		THIS->mLatency -= delta;
#if DEBUG
		os_log_debug(OS_LOG_DEFAULT, "latency = %.0f\n", THIS->mLatency);
#endif
		*ioActionFlags = kAudioUnitRenderAction_OutputIsSilence;
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex)
			memset(static_cast<int8_t *>(ioData->mBuffers[bufferIndex].mData), 0, ioData->mBuffers[bufferIndex].mDataByteSize);
		return noErr;
	}

	auto result = AudioUnitRender(THIS->mMixerUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
	assert(result == noErr);

	return noErr;
}

OSStatus SFBAUv2IO::MixerInputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(inRefCon);

	auto adjustedTimeStamp = inTimeStamp->mSampleTime - THIS->mLatency;
	if(!THIS->mInputRingBuffer.Read(ioData, inNumberFrames, adjustedTimeStamp)) {
		os_log_debug(OS_LOG_DEFAULT, "SFBCARingBuffer::Read failed at sample time %.0f", adjustedTimeStamp);
		*ioActionFlags = kAudioUnitRenderAction_OutputIsSilence;
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex)
		memset(static_cast<int8_t *>(ioData->mBuffers[bufferIndex].mData), 0, ioData->mBuffers[bufferIndex].mDataByteSize);
		int64_t startTime, endTime;
		if(THIS->mInputRingBuffer.GetTimeBounds(startTime, endTime))
			THIS->mLatency = inTimeStamp->mSampleTime - startTime;
	}

	return noErr;
}

void SFBAUv2IO::ScheduledAudioSliceCompletionProc(void *userData, ScheduledAudioSlice *slice)
{
//	SFBAUv2IO *THIS = static_cast<SFBAUv2IO *>(userData);
	static_cast<SFBScheduledAudioSlice *>(slice)->mAvailable = true;
}
