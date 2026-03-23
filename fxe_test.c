#include <stdio.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

int main(void) {
    printf("FakeIrisXE Hardware Test\n========================\n\n");

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("FakeIrisXEFramebuffer"));
    if (!svc) { fprintf(stderr, "Service not found\n"); return 1; }
    printf("Found service\n");

    printf("Triggering requestProbe...\n");
    kern_return_t kr = IOServiceRequestProbe(svc, 0);
    printf("requestProbe returned: 0x%08x\n\n", kr);

    /* Give it a moment */
    usleep(500000);

    printf("Results from IORegistry:\n");
    const char *keys[] = {
        "FXE-Test-ForcewakeOK", "FXE-Test-PW1OK", "FXE-Test-PW2OK",
        "FXE-Test-CDClk-Before", "FXE-Test-CDClk-After", "FXE-Test-DSSM",
        "FXE-Test-Done", NULL
    };
    for (int i = 0; keys[i]; i++) {
        CFTypeRef val = IORegistryEntryCreateCFProperty(svc,
            CFStringCreateWithCStringNoCopy(NULL, keys[i], kCFStringEncodingUTF8, NULL),
            kCFAllocatorDefault, 0);
        if (val) {
            printf("  %-30s = ", keys[i]);
            if (CFGetTypeID(val) == CFBooleanGetTypeID())
                printf("%s\n", CFBooleanGetValue((CFBooleanRef)val) ? "YES ✅" : "NO ❌");
            else if (CFGetTypeID(val) == CFNumberGetTypeID()) {
                uint64_t n; CFNumberGetValue((CFNumberRef)val, kCFNumberLongLongType, &n);
                printf("0x%llx\n", n);
            }
            CFRelease(val);
        }
    }

    IOObjectRelease(svc);
    return 0;
}
