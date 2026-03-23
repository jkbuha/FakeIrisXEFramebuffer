#!/bin/bash
# Trigger HW test via requestProbe and read results from IORegistry
echo "=== FakeIrisXE Hardware Test ==="
echo "Triggering requestProbe..."
# IOKitWaitQuiet after probe
sudo ioreg -l -p IOService -n FakeIrisXEFramebuffer 2>/dev/null | grep "FXE-Test" | head -10

# Use io_service_t requestProbe via ioreg hint
# Actually use the IOFramebuffer probe mechanism via display reconfigure
sudo /System/Library/Frameworks/CoreServices.framework/Frameworks/OSServices.framework/Versions/A/Support/ioreg -w 0 -l -p IOService -n FakeIrisXEFramebuffer 2>/dev/null | grep "FXE-" | head -20
