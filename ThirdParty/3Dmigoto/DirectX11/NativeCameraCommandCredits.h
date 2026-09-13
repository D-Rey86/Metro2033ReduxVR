#pragma once

// Camera metadata only. Never writes a Metro camera or a command buffer.
// The native adapter must serialize Record/TakeReplay and assign epochs at
// actual ownership transitions. No locks are held while executing Metro code.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

namespace NativeCameraCredits {

struct Credit {
    uint64_t epoch;
    uint64_t generation;
    float nativeView[16];
    float yaw;
    float pitch;
    bool valid;
};

struct Command {
    unsigned payloadOffset;
    Credit credit;
};

// Metro's observed command storage limit is 0x2000 DWORDs. Even without
// alignment padding, at most floor(0x8000 / 68) camera commands fit.
static const unsigned kBufferBytes = 0x8000;
static const unsigned kCommands = kBufferBytes / 68;
static const unsigned kBuffers = 64;

inline bool SameNativeView(const float *a, const float *b)
{
    for (unsigned i = 0; i < 16; ++i)
        if (!isfinite(a[i]) || !isfinite(b[i]) || a[i] != b[i]) return false;
    return true;
}

// Replay storage belongs to the caller's invocation, not to the reusable
// native buffer. Nested replay needs a separate instance per nesting level.
struct Replay {
    uint64_t serial;
    unsigned count;
    unsigned next;
    bool poisoned;
    Command commands[kCommands];

    void Clear() { serial = 0; count = next = 0; poisoned = false; }
    bool Complete() const { return !poisoned && next == count; }

    bool Consume(const float *nativeView, uint64_t epoch, Credit *out)
    {
        if (poisoned || next == count) {
            poisoned = true;
            return false;
        }
        const Credit &value = commands[next++].credit;
        // Every camera opcode must have an entry, including untracked views.
        // Never search ahead for a similar matrix: equal pictures can have
        // different body/head decomposition and cancellation credit.
        if (!SameNativeView(value.nativeView, nativeView)) {
            poisoned = true;
            return false;
        }
        if (!value.valid || value.epoch != epoch) return false;
        *out = value;
        return true;
    }
};

struct Buffer {
    uintptr_t address;
    uint64_t serial;
    unsigned count;
    bool poisoned;
    Command commands[kCommands];
};

class Commands {
    Buffer buffers[kBuffers];
    uint64_t nextSerial;
    bool failed;
public:
    Commands() : nextSerial(0), failed(false) { Clear(); }
    void Clear()
    {
        for (unsigned i = 0; i < kBuffers; ++i) buffers[i].address = 0;
        failed = false;
        // Serial deliberately stays monotonic across Clear/epoch changes.
    }
    bool Failed() const { return failed; }
    void Fail() { failed = true; }

    bool Record(uintptr_t buffer, unsigned payloadOffset, const Credit &credit)
    {
        if (failed) return false;
        if (!buffer || payloadOffset < 4 || payloadOffset > kBufferBytes - 64 ||
            (payloadOffset & 15)) {
            failed = true;
            return false;
        }
        Buffer *slot = NULL, *freeSlot = NULL;
        for (unsigned i = 0; i < kBuffers; ++i) {
            if (buffers[i].address == buffer) { slot = &buffers[i]; break; }
            if (!buffers[i].address && !freeSlot) freeSlot = &buffers[i];
        }
        if (!slot) {
            if (!freeSlot) { failed = true; return false; }
            slot = freeSlot;
            slot->address = buffer;
            slot->serial = ++nextSerial;
            slot->count = 0;
            slot->poisoned = false;
        }
        // Reuse without an observed replay/retirement is NOT a new lifetime
        // we can safely guess. Keep it unknown until the adapter resets.
        if (slot->count == kCommands || (slot->count &&
            payloadOffset < slot->commands[slot->count - 1].payloadOffset + 68)) {
            failed = true;
            slot->poisoned = true;
            return false;
        }
        Command &cmd = slot->commands[slot->count++];
        cmd.payloadOffset = payloadOffset;
        cmd.credit = credit;
        return true;
    }

    bool TakeReplay(uintptr_t buffer, Replay *replay)
    {
        replay->Clear();
        if (!buffer) { replay->poisoned = true; return false; }
        if (failed) { replay->poisoned = true; return false; }
        for (unsigned i = 0; i < kBuffers; ++i) {
            Buffer &slot = buffers[i];
            if (slot.address != buffer) continue;
            replay->serial = slot.serial;
            replay->count = slot.count;
            replay->poisoned = slot.poisoned;
            memcpy(replay->commands, slot.commands, slot.count * sizeof(Command));
            slot.address = 0;
            return !replay->poisoned;
        }
        // A batch with no camera commands is legal. The caller retains its
        // existing camera state; an unexpected camera setter poisons Consume.
        return true;
    }
};

} // namespace NativeCameraCredits
