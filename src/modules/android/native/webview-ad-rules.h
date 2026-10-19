#pragma once
#import <Foundation/Foundation.h>
#include "network-policy.h"

static NSArray *klj_webview_ad_rules(void){
    size_t count=0;const char *const *hosts=klj_network_ad_hosts(&count);
    NSMutableArray *rules=[NSMutableArray arrayWithCapacity:count];
    for(size_t i=0;i<count;i++){
        NSString *host=[[NSString stringWithUTF8String:hosts[i]] stringByReplacingOccurrencesOfString:@"." withString:@"\\."];
        NSString *pattern=[NSString stringWithFormat:@"^[a-z][a-z0-9+.-]*://([^/@]+@)?([^/:]+\\.)?%@\\.*(:[0-9]+)?([/?#].*)?$",host];
        [rules addObject:@{@"trigger":@{@"url-filter":pattern,@"url-filter-is-case-sensitive":@NO},@"action":@{@"type":@"block"}}];
    }
    return rules;
}
