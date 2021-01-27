/*
 * Copyright (c) 2021 Stephen F. Booth <me@sbooth.org>
 * MIT license
 */

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

	NSURL *inputRecordingURL = [temporaryDirectory URLByAppendingPathComponent:@"input_recording.caf"];
	NSLog(@"Recording input audio unit output to %@", inputRecordingURL);
	_audioIO->SetInputRecordingPath((__bridge CFURLRef)inputRecordingURL, kAudioFileCAFType, SFBAudioFormat(kSFBCommonPCMFormatInt16, 44100, 2, true));

	NSURL *playerRecordingURL = [temporaryDirectory URLByAppendingPathComponent:@"player_recording.caf"];
	NSLog(@"Recording player audio unit output to %@", playerRecordingURL);
	_audioIO->SetPlayerRecordingPath((__bridge CFURLRef)playerRecordingURL, kAudioFileCAFType, SFBAudioFormat(kSFBCommonPCMFormatInt16, 44100, 2, true));

	NSURL *outputRecordingURL = [temporaryDirectory URLByAppendingPathComponent:@"output_recording.caf"];
	NSLog(@"Recording output audio unit output to %@", outputRecordingURL);
	_audioIO->SetOutputRecordingPath((__bridge CFURLRef)outputRecordingURL, kAudioFileCAFType, SFBAudioFormat(kSFBCommonPCMFormatInt16, 44100, 2, true));
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
		_audioIO->Play((__bridge CFURLRef)u);
	}
}

@end
