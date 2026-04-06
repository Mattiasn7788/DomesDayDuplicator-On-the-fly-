#import <AVFoundation/AVFoundation.h>
#include "macos_permissions.h"

void requestMicrophonePermission(std::function<void(bool)> callback)
{
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
        completionHandler:^(BOOL granted) {
            callback(granted);
        }];
}
