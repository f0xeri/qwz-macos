/* Repair saved QwzAirport service names after development builds.
 * Public SystemConfiguration/IOKit APIs; no interface detach, radio or Keychain access.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#import <Foundation/Foundation.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <IOKit/IOKitLib.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *string(CFStringRef s) { return (__bridge NSString *)s; }
static void require(BOOL ok, const char *operation)
{
    if (!ok) {
        fprintf(stderr, "%s: %s\n", operation, SCErrorString(SCError()));
        exit(1); // Uncommitted SCPreferences edits disappear with this process.
    }
}

int main(int argc, const char **argv) { @autoreleasepool {
    BOOL apply = argc == 3 && strcmp(argv[1], "--apply") == 0;
    if (!(argc == 1 || apply)) {
        fprintf(stderr, "usage: %s [--apply /absolute/new-backup.plist]\n", argv[0]);
        return 2;
    }
    if (apply && (geteuid() != 0 || argv[2][0] != '/')) {
        fprintf(stderr, "--apply needs root and an absolute backup path\n");
        return 2;
    }
    io_service_t native = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching("QwzAirportInterface"));
    if (!native) { fprintf(stderr, "No running QwzAirport interface\n"); return 1; }
    NSString *bsd = CFBridgingRelease(IORegistryEntryCreateCFProperty(native,
        CFSTR("IOInterfaceName"), kCFAllocatorDefault, 0));
    IOObjectRelease(native);
    if (![bsd isKindOfClass:NSString.class] || ![bsd hasPrefix:@"en"]) return 1;

    NSArray *interfaces = CFBridgingRelease(SCNetworkInterfaceCopyAll());
    NSMutableSet *present = [NSMutableSet set];
    BOOL nativeWiFi = NO;
    for (id obj in interfaces) {
        SCNetworkInterfaceRef i = (__bridge SCNetworkInterfaceRef)obj;
        NSString *name = string(SCNetworkInterfaceGetBSDName(i));
        if (name) [present addObject:name];
        if ([name isEqual:bsd]) nativeWiFi = CFEqual(SCNetworkInterfaceGetInterfaceType(i),
            kSCNetworkInterfaceTypeIEEE80211);
    }
    if (!nativeWiFi) { fprintf(stderr, "Current endpoint is not native Wi-Fi\n"); return 1; }

    // Use persisted IO paths to identify our old native endpoints, never names
    // or MAC addresses. Keep the independent QwzWifi/HeliPort Ethernet endpoint.
    NSDictionary *catalog = [NSDictionary dictionaryWithContentsOfFile:
        @"/Library/Preferences/SystemConfiguration/NetworkInterfaces.plist"];
    NSMutableSet *oldNative = [NSMutableSet set];
    for (NSDictionary *entry in catalog[@"Interfaces"]) {
        NSString *path = entry[@"IOPathMatch"], *name = entry[@"BSD Name"];
        if ([path isKindOfClass:NSString.class] && [name isKindOfClass:NSString.class] &&
            [path containsString:@"/QwzWifi/QwzAirportNub/QwzAirport/"] &&
            ![present containsObject:name]) [oldNative addObject:name];
    }
    SCPreferencesRef prefs = SCPreferencesCreate(NULL, CFSTR("Qwz service repair"), NULL);
    require(prefs != NULL, "Open preferences");
    if (apply) require(SCPreferencesLock(prefs, FALSE), "Lock preferences");
    NSArray *services = CFBridgingRelease(SCNetworkServiceCopyAll(prefs));
    NSMutableArray *obsolete = [NSMutableArray array];
    SCNetworkServiceRef active = NULL;
    for (id obj in services) {
        SCNetworkServiceRef s = (__bridge SCNetworkServiceRef)obj;
        SCNetworkInterfaceRef i = SCNetworkServiceGetInterface(s);
        NSString *name = string(SCNetworkInterfaceGetBSDName(i));
        if ([name isEqual:bsd]) {
            if (active) { fprintf(stderr, "Ambiguous current services; no changes\n"); return 1; }
            active = s;
        } else if ([oldNative containsObject:name]) [obsolete addObject:obj];
    }
    if (!active) { fprintf(stderr, "No saved service for current native endpoint\n"); return 1; }
    printf("Keep native %s: %s -> Wi-Fi (same service ID, IP/DNS settings)\n",
        bsd.UTF8String, string(SCNetworkServiceGetName(active)).UTF8String);
    for (id obj in obsolete) {
        SCNetworkServiceRef s = (__bridge SCNetworkServiceRef)obj;
        printf("Remove absent native service %s (%s, %s)\n",
            string(SCNetworkServiceGetName(s)).UTF8String,
            string(SCNetworkInterfaceGetBSDName(SCNetworkServiceGetInterface(s))).UTF8String,
            string(SCNetworkServiceGetServiceID(s)).UTF8String);
    }
    if (!apply) { puts("Read-only preview; --apply requires a new backup path."); CFRelease(prefs); return 0; }

    // Back up the exact locked preferences before any mutation. A new file is
    // mandatory; owner-only permissions also cover custom proxy configuration.
    NSMutableDictionary *snapshot = [NSMutableDictionary dictionary];
    NSArray *keys = CFBridgingRelease(SCPreferencesCopyKeyList(prefs));
    for (NSString *key in keys) snapshot[key] = (__bridge id)SCPreferencesGetValue(prefs, (__bridge CFStringRef)key);
    NSError *error = nil;
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:snapshot
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:&error];
    mode_t mask = umask(077);
    BOOL saved = data && [data writeToFile:@(argv[2]) options:NSDataWritingWithoutOverwriting error:&error];
    umask(mask);
    if (!saved) { fprintf(stderr, "Backup failed: %s\n", error.localizedDescription.UTF8String); return 1; }
    printf("Backup: %s\n", argv[2]);
    NSArray *sets = CFBridgingRelease(SCNetworkSetCopyAll(prefs));
    for (id obj in obsolete) {
        SCNetworkServiceRef s = (__bridge SCNetworkServiceRef)obj;
        for (id setObj in sets) {
            SCNetworkSetRef set = (__bridge SCNetworkSetRef)setObj;
            NSArray *members = CFBridgingRelease(SCNetworkSetCopyServices(set));
            for (id member in members) {
                if (CFEqual(SCNetworkServiceGetServiceID((__bridge SCNetworkServiceRef)member),
                    SCNetworkServiceGetServiceID(s))) require(SCNetworkSetRemoveService(set, s), "Remove from set");
            }
        }
        require(SCNetworkServiceRemove(s), "Remove obsolete service");
    }
    require(SCNetworkServiceSetName(active, CFSTR("Wi-Fi")), "Name native service");
    require(SCPreferencesCommitChanges(prefs), "Commit preferences");
    require(SCPreferencesApplyChanges(prefs), "Apply preferences (backup retained if apply fails)");
    SCPreferencesUnlock(prefs);
    CFRelease(prefs);
    puts("Applied. Other interfaces and saved Wi-Fi profiles were preserved.");
    return 0;
} }
