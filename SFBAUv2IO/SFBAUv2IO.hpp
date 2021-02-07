//
// Copyright (c) 2021 Stephen F. Booth <me@sbooth.org>
// MIT license
//

#pragma once

#import <atomic>
#import <memory>
#import <vector>

#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>

#import "SFBCABufferList.hpp"
#import "SFBCARingBuffer.hpp"
#import "SFBHALAudioDevice.hpp"

namespace SFB {
	class AudioUnitRecorder;
}
class SFBScheduledAudioSlice;

class SFBAUv2IO
{
	
public:

	/// Creates a new @c SFBAUv2IO for the default system input and output devices
	SFBAUv2IO();

	// This class is non-copyable
	SFBAUv2IO(const SFBAUv2IO& rhs) = delete;

	// This class is non-assignable
	SFBAUv2IO& operator=(const SFBAUv2IO& rhs) = delete;

	~SFBAUv2IO();

	// This class is non-movable
	SFBAUv2IO(SFBAUv2IO&& rhs) = delete;

	// This class is non-move assignable
	SFBAUv2IO& operator=(SFBAUv2IO&& rhs) = delete;


	SFBAUv2IO(AudioObjectID inputDeviceID, AudioObjectID outputDeviceID);


	SFB::HALAudioDevice InputDevice() const;
	SFB::HALAudioDevice OutputDevice() const;

	void Start();
	void StartAt(const AudioTimeStamp& timeStamp);
	void Stop();

	bool IsRunning() const;
	bool OutputIsRunning() const;
	bool InputIsRunning() const;

	void Play(CFURLRef url);
	void PlayAt(CFURLRef url, const AudioTimeStamp& timeStamp);

	void GetInputFormat(AudioStreamBasicDescription& format);
	void GetPlayerFormat(AudioStreamBasicDescription& format);
	void GetOutputFormat(AudioStreamBasicDescription& format);

	void SetInputRecordingURL(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format);
	void SetPlayerRecordingURL(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format);
	void SetOutputRecordingURL(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format);

private:

	void Initialize(AudioObjectID inputDeviceID, AudioObjectID outputDeviceID);

	void CreateInputAU(AudioObjectID inputDeviceID);
	void CreateOutputAU(AudioObjectID outputDeviceID);
	void CreateMixerAU();
	void CreatePlayerAU();
	void BuildGraph();

	UInt32 MinimumInputLatency() const;
	UInt32 MinimumOutputLatency() const;
	inline UInt32 MinimumThroughLatency() const
	{
		return MinimumOutputLatency() + MinimumInputLatency();
	}

	std::unique_ptr<SFB::AudioUnitRecorder> mInputRecorder;
	std::unique_ptr<SFB::AudioUnitRecorder> mPlayerRecorder;
	std::unique_ptr<SFB::AudioUnitRecorder> mOutputRecorder;

	AudioUnit mInputUnit;
	AudioUnit mPlayerUnit;
	AudioUnit mMixerUnit;
	AudioUnit mOutputUnit;

	std::atomic<double> mFirstInputSampleTime;
	std::atomic<double> mFirstOutputSampleTime;
	Float64 mThroughLatency;

	SFB::CABufferList mInputBufferList;
	SFB::CARingBuffer mInputRingBuffer;

	static OSStatus InputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);
	static OSStatus OutputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);

	static OSStatus MixerInputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);

	SFBScheduledAudioSlice *mScheduledAudioSlices;
	
	static void ScheduledAudioSliceCompletionProc(void *userData, ScheduledAudioSlice *slice);

};
