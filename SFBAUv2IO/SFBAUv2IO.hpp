/*
 * Copyright (c) 2021 Stephen F. Booth <me@sbooth.org>
 * MIT license
 */

#pragma once

#import <atomic>
#import <memory>
#import <vector>

#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>

#import "SFBAudioBufferList.hpp"
#import "SFBCARingBuffer.hpp"

class SFBAudioUnitRecorder;
class SFBScheduledAudioSlice;

class SFBAUv2IO
{
	
public:

	SFBAUv2IO();
	SFBAUv2IO(AudioObjectID inputDevice, AudioObjectID outputDevice);

	~SFBAUv2IO();

	void Start();
	void StartAt(const AudioTimeStamp& startTime);
	void Stop();

	bool IsRunning() const;
	bool OutputIsRunning() const;
	bool InputIsRunning() const;

	void Play(CFURLRef url);
	void PlayAt(CFURLRef url, const AudioTimeStamp& startTime);

	void GetInputFormat(AudioStreamBasicDescription& format);
	void GetPlayerFormat(AudioStreamBasicDescription& format);
	void GetOutputFormat(AudioStreamBasicDescription& format);

	void SetInputRecordingPath(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format);
	void SetPlayerRecordingPath(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format);
	void SetOutputRecordingPath(CFURLRef url, AudioFileTypeID fileType, const AudioStreamBasicDescription& format);

private:

	void Initialize(AudioObjectID inputDevice, AudioObjectID outputDevice);

	void CreateInputAU(AudioObjectID inputDevice);
	void CreateOutputAU(AudioObjectID outputDevice);
	void CreateMixerAU();
	void CreatePlayerAU();
	void BuildGraph();

	UInt32 MinimumInputLatency() const;
	UInt32 MinimumOutputLatency() const;
	inline UInt32 MinimumThroughLatency() const
	{
		return MinimumInputLatency() + MinimumOutputLatency();
	}

	std::unique_ptr<SFBAudioUnitRecorder> mInputRecorder;
	std::unique_ptr<SFBAudioUnitRecorder> mPlayerRecorder;
	std::unique_ptr<SFBAudioUnitRecorder> mOutputRecorder;

	AudioUnit mInputUnit;
	AudioUnit mPlayerUnit;
	AudioUnit mMixerUnit;
	AudioUnit mOutputUnit;

	std::atomic<double> mFirstInputTime;
	std::atomic<double> mFirstOutputTime;
	Float64 mLatency;

	SFBAudioBufferList mInputBufferList;
	SFBCARingBuffer mInputRingBuffer;

	static OSStatus InputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);
	static OSStatus OutputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);

	static OSStatus MixerInputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);

	SFBScheduledAudioSlice *mScheduledAudioSlices;
	
	static void ScheduledAudioSliceCompletionProc(void *userData, ScheduledAudioSlice *slice);

};
