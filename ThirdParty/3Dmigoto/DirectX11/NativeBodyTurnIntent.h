#pragma once

// A bounded one-shot mailbox, not a native rate accumulator. The controller
// publisher and native-look consumer serialize access externally. A newer
// smooth sample supersedes an unconsumed old sample; blocked updates discard
// rather than replaying a backlog when camera ownership returns.
namespace NativeBodyTurn {
class Intent {
    int units;
    bool snap;
public:
    Intent() : units(0), snap(false) {}
    void Clear() { units = 0; snap = false; }
    void Publish(int value, bool held, bool isSnap)
    {
        if (!held) { Clear(); return; }
        if (!isSnap || !snap || value != 0) units = value;
        snap = isSnap;
    }
    int Take() { const int result = units; units = 0; return result; }
};
} // namespace NativeBodyTurn
