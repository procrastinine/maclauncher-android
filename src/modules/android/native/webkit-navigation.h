#pragma once
#import <Foundation/Foundation.h>

// Keep the failed request's origin when WebKit has no committed document URL.
inline NSURL* klj_webkit_failure_url(NSError* error,NSURLRequest* request){
    id value=error.userInfo[NSURLErrorFailingURLErrorKey];
    if([value isKindOfClass:NSURL.class])return value;
    // Older WebKit errors can retain the original string field.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    value=error.userInfo[NSURLErrorFailingURLStringErrorKey];
#pragma clang diagnostic pop
    if([value isKindOfClass:NSString.class]&&[value length]){
        NSURL* url=[NSURL URLWithString:value];if(url)return url;
    }
    return request.URL;
}

inline NSString* klj_webkit_android_url(NSURL* url){
    NSString*scheme=url.scheme.lowercaseString;
    NSString*root=[scheme isEqualToString:@"android-asset"]?@"file:///android_asset":[scheme isEqualToString:@"android-resource"]?@"file:///android_res":nil;
    if(!root)return url.absoluteString;
    NSURLComponents*parts=[NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
    return [NSString stringWithFormat:@"%@%@%@%@",root,parts.percentEncodedPath?:@"/",parts.percentEncodedQuery?[@"?" stringByAppendingString:parts.percentEncodedQuery]:@"",parts.percentEncodedFragment?[@"#" stringByAppendingString:parts.percentEncodedFragment]:@""];
}
