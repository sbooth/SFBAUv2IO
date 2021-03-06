//
// Copyright (c) 2021 Stephen F. Booth <me@sbooth.org>
// MIT license
//

#import <memory>

#import "ViewController.h"

#import "SFBAUv2IO.hpp"

@interface ViewController ()
{
	std::unique_ptr<SFBAUv2IO> _audioIO;
}
@end

@implementation ViewController

- (void)viewDidLoad {
	[super viewDidLoad];

	_audioIO = std::make_unique<SFBAUv2IO>();

	NSURL *temporaryDirectory = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];

	SFB::CAStreamBasicDescription format;
	_audioIO->GetInputFormat(format);

	NSURL *inputRecordingURL = [temporaryDirectory URLByAppendingPathComponent:@"input_recording.caf"];
//	NSLog(@"Recording input audio unit output to %@", inputRecordingURL);
	_audioIO->SetInputRecordingURL((__bridge CFURLRef)inputRecordingURL, kAudioFileCAFType, SFB::CAStreamBasicDescription(SFB::CommonPCMFormat::int16, format.mSampleRate, format.ChannelCount(), true));

	_audioIO->GetPlayerFormat(format);
	NSURL *playerRecordingURL = [temporaryDirectory URLByAppendingPathComponent:@"player_recording.caf"];
//	NSLog(@"Recording player audio unit output to %@", playerRecordingURL);
	_audioIO->SetPlayerRecordingURL((__bridge CFURLRef)playerRecordingURL, kAudioFileCAFType, SFB::CAStreamBasicDescription(SFB::CommonPCMFormat::int16, format.mSampleRate, format.ChannelCount(), true));

	_audioIO->GetOutputFormat(format);
	NSURL *outputRecordingURL = [temporaryDirectory URLByAppendingPathComponent:@"output_recording.caf"];
//	NSLog(@"Recording output audio unit output to %@", outputRecordingURL);
	_audioIO->SetOutputRecordingURL((__bridge CFURLRef)outputRecordingURL, kAudioFileCAFType, SFB::CAStreamBasicDescription(SFB::CommonPCMFormat::int16, format.mSampleRate, format.ChannelCount(), true));
}

- (IBAction)start:(id)sender {
	if(!_audioIO->IsRunning()) {
		NSLog(@"⏺");
//		AudioTimeStamp ts;
//		FillOutAudioTimeStampWithHostTime(ts, AudioGetCurrentHostTime() + AudioConvertNanosToHostTime(NSEC_PER_SEC >> 2));
//		_audioIO->StartAt(ts);
		_audioIO->Start();
	}
}

- (IBAction)stop:(id)sender {
	if(_audioIO->IsRunning()) {
		_audioIO->Stop();
		NSLog(@"⏹");
	}
}

- (IBAction)play:(id)sender {
	if(_audioIO->IsRunning()) {
		NSLog(@"▶️");
		NSURL *u = [[NSBundle mainBundle] URLForResource:@"Tones" withExtension:@"wav"];
//		AudioTimeStamp ts;
//		FillOutAudioTimeStampWithHostTime(ts, AudioGetCurrentHostTime() + AudioConvertNanosToHostTime(NSEC_PER_SEC >> 1));
//		_audioIO->PlayAt((__bridge CFURLRef)u, ts);
		_audioIO->Play((__bridge CFURLRef)u);
	}
}

@end
