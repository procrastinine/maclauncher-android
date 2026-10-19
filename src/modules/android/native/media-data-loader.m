// AVFoundation range requests use the extractor's retained callback source.
// The extractor owns close; this delegate retains only the callback lifetime.
#import <AVFoundation/AVFoundation.h>
#include "media-data-source.h"

@interface KljMediaDataLoader : NSObject <AVAssetResourceLoaderDelegate> {
@public klj_media_source *source;
}
@end
@implementation KljMediaDataLoader
- (void)dealloc {
    klj_media_source_delete(source);

}
- (BOOL)resourceLoader:(AVAssetResourceLoader *)loader shouldWaitForLoadingOfRequestedResource:(AVAssetResourceLoadingRequest *)request {
    (void)loader;
    @autoreleasepool {
        if(klj_media_source_closed(source)){
            [request finishLoadingWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]];return YES;
        }
        ssize_t length=klj_media_source_size(source);
        AVAssetResourceLoadingContentInformationRequest *info=request.contentInformationRequest;
        if(info){
            info.byteRangeAccessSupported=YES;
            if(length>=0)info.contentLength=length;
            unsigned char signature[12]={0};
            ssize_t got=klj_media_source_read(source,0,signature,sizeof signature);
            if(got==12&&!memcmp(signature+4,"ftyp",4))
                info.contentType=!memcmp(signature+8,"qt  ",4)?@"com.apple.quicktime-movie":@"public.mpeg-4";
        }
        AVAssetResourceLoadingDataRequest *data=request.dataRequest;
        if(data){
            int64_t offset=data.currentOffset;
            int64_t remaining=data.requestsAllDataToEndOfResource?(length>=0?length-offset:INT64_MAX):data.requestedLength-(offset-data.requestedOffset);
            unsigned char buffer[64*1024];
            while(remaining>0&&!request.cancelled&&!klj_media_source_closed(source)){
                size_t count=(size_t)MIN(remaining,(int64_t)sizeof buffer);
                ssize_t got=klj_media_source_read(source,offset,buffer,count);
                if(got<-1){[request finishLoadingWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotDecodeContentData userInfo:nil]];return YES;}
                if(got<=0)break;
                [data respondWithData:[NSData dataWithBytes:buffer length:(NSUInteger)got]];
                offset+=got;remaining-=got;
            }
        }
        if(klj_media_source_closed(source))
            [request finishLoadingWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]];
        else if(!request.cancelled)[request finishLoading];
        return YES;
    }
}
@end

// Retained until the demux reader, asset and outstanding requests are released.
static AVURLAsset *klj_media_data_asset(klj_media_source *source,void **owner){
    if(!source||klj_media_source_closed(source))return nil;
    KljMediaDataLoader *loader=[[KljMediaDataLoader alloc] init];
    loader->source=source;klj_media_source_retain(source);
    NSURL *url=[NSURL URLWithString:[@"maclauncher-media://source/" stringByAppendingString:NSUUID.UUID.UUIDString]];
    AVURLAsset *asset=[AVURLAsset URLAssetWithURL:url options:nil];
    dispatch_queue_t queue=dispatch_queue_create("maclauncher.android.media",DISPATCH_QUEUE_SERIAL);
    [asset.resourceLoader setDelegate:loader queue:queue];

    *owner=(void*)CFBridgingRetain(loader);
    return asset;
}
