#import <Foundation/Foundation.h>
#include "webkit-navigation.h"

int main(){@autoreleasepool{
    NSMutableArray* records=[NSMutableArray new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSString* legacyURLKey=NSURLErrorFailingURLStringErrorKey;
#pragma clang diagnostic pop
    NSMutableURLRequest* pending=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"https://pending.example/submit"]];
    pending.HTTPMethod=@"POST";pending.HTTPBody=[@"preserved=body" dataUsingEncoding:NSUTF8StringEncoding];
    auto check=[&](NSString* label,NSDictionary* info,NSURLRequest* request,NSString* expected){
        NSError* error=[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotConnectToHost userInfo:info];
        NSString* address=klj_webkit_android_url(klj_webkit_failure_url(error,request));
        if(![address isEqualToString:expected]){fprintf(stderr,"%s: %s != %s\n",label.UTF8String,address.UTF8String,expected.UTF8String);abort();}
        [records addObject:@{@"case":label,@"url":address,@"expected":expected,@"description":error.localizedDescription}];
    };
    check(@"provisional",@{NSURLErrorFailingURLErrorKey:[NSURL URLWithString:@"https://failed.example/first"],legacyURLKey:@"https://ignored.example/"},pending,@"https://failed.example/first");
    check(@"committed",@{NSURLErrorFailingURLErrorKey:[NSURL URLWithString:@"https://failed.example/after-commit?q=%F0%9F%90%88"]},pending,@"https://failed.example/after-commit?q=%F0%9F%90%88");
    check(@"error-string",@{legacyURLKey:@"https://failed.example/string"},pending,@"https://failed.example/string");
    check(@"request-origin",@{},pending,@"https://pending.example/submit");
    check(@"malformed-error-field",@{NSURLErrorFailingURLErrorKey:@17},pending,@"https://pending.example/submit");
    check(@"android-resource-origin",@{NSURLErrorFailingURLErrorKey:[NSURL URLWithString:@"android-asset:///missing%20page.html?q=1#section"]},pending,@"file:///android_asset/missing%20page.html?q=1#section");
    if(![pending.HTTPMethod isEqualToString:@"POST"]||![pending.HTTPBody isEqualToData:[@"preserved=body" dataUsingEncoding:NSUTF8StringEncoding]])abort();
    NSError* unknown=[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorUnknown userInfo:nil];
    if(klj_webkit_failure_url(unknown,nil)!=nil)abort();
    NSData* json=[NSJSONSerialization dataWithJSONObject:records options:0 error:nil];
    fwrite(json.bytes,1,json.length,stdout);fputc('\n',stdout);
}}
