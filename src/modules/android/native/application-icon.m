#import <AppKit/AppKit.h>
#include <stdio.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *application = [NSString stringWithUTF8String:argv[1]];
        NSString *resource = [application stringByAppendingPathComponent:@"Contents/Resources/Application.icns"];
        NSImage *icon = [[NSImage alloc] initWithContentsOfFile:resource];
        if (!icon || ![[NSWorkspace sharedWorkspace] setIcon:icon forFile:application options:0]) {
            fputs("The application icon could not be assigned.\n", stderr);
            return 1;
        }
        return 0;
    }
}
