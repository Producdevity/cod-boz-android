#pragma once

#include "guest_address.hpp"

#include <cstddef>
#include <cstdint>

// Guest ABI structures. These must be byte-identical to what the ARM32 game
// expects. Guest pointers are GuestAddr (uint32_t), so the layouts are stable on
// a 64-bit host. Offsets are load-bearing and asserted.
namespace codboz::guest {

// s3eInetAddress. Offsets verified against the reference loader.
struct InetAddress {
    int32_t type;             // 0x00
    char local;               // 0x04
    char local_address[128];  // 0x05
    char abstract;            // 0x85
    uint8_t pad0[2];          // 0x86
    uint32_t ip_address;      // 0x88 (network byte order)
    uint8_t ipv6_address[16]; // 0x8c
    uint16_t port;            // 0x9c (network byte order)
    char string[128];         // 0x9e
    uint8_t pad1[2];          // 0x11e
    uint32_t next;            // 0x120
};
static_assert(offsetof(InetAddress, ip_address) == 0x88, "InetAddress ip offset");
static_assert(offsetof(InetAddress, ipv6_address) == 0x8c, "InetAddress ipv6 offset");
static_assert(offsetof(InetAddress, port) == 0x9c, "InetAddress port offset");
static_assert(offsetof(InetAddress, next) == 0x120, "InetAddress next offset");
static_assert(sizeof(InetAddress) == 0x124, "InetAddress size");

// s3eZeroConfFoundData (size 0x38).
struct ZeroConfFound {
    GuestAddr service_id;     // 0x00
    uint32_t reserved04;      // 0x04
    GuestAddr name;           // 0x08
    GuestAddr service_type;   // 0x0c
    GuestAddr domain;         // 0x10
    GuestAddr host;           // 0x14
    uint16_t port;            // 0x18
    uint16_t txt_count;       // 0x1a
    GuestAddr txt_records;    // 0x1c
    uint32_t ipv4_address;    // 0x20
    uint8_t reserved24[0x14]; // 0x24
};
static_assert(offsetof(ZeroConfFound, name) == 0x08, "ZeroConfFound name offset");
static_assert(offsetof(ZeroConfFound, port) == 0x18, "ZeroConfFound port offset");
static_assert(offsetof(ZeroConfFound, txt_records) == 0x1c, "ZeroConfFound txt offset");
static_assert(offsetof(ZeroConfFound, ipv4_address) == 0x20, "ZeroConfFound ipv4 offset");
static_assert(sizeof(ZeroConfFound) == 0x38, "ZeroConfFound size");

// s3eZeroConfTxtUpdateData (size 0x0c).
struct ZeroConfTxtUpdate {
    GuestAddr service_id;  // 0x00
    uint16_t reserved04;   // 0x04
    uint16_t txt_count;    // 0x06
    GuestAddr txt_records; // 0x08
};
static_assert(offsetof(ZeroConfTxtUpdate, txt_count) == 0x06, "ZeroConfTxt count offset");
static_assert(offsetof(ZeroConfTxtUpdate, txt_records) == 0x08, "ZeroConfTxt records offset");
static_assert(sizeof(ZeroConfTxtUpdate) == 0x0c, "ZeroConfTxt size");

// Pointer/touch/keyboard event payloads passed to guest callbacks. All scalar.
struct PointerButtonEvent {
    int32_t button;
    int32_t pressed;
    int32_t x;
    int32_t y;
};
struct PointerMotionEvent {
    int32_t x;
    int32_t y;
};
struct PointerTouchEvent {
    int32_t touch_id;
    int32_t pressed;
    int32_t x;
    int32_t y;
};
struct PointerTouchMotionEvent {
    int32_t touch_id;
    int32_t x;
    int32_t y;
};
struct KeyboardEvent {
    int32_t key;
    int32_t pressed;
};
struct TouchpadButtonEvent {
    int32_t id;
    int32_t pressed;
    int32_t x;
    int32_t y;
};
struct TouchpadMotionEvent {
    int32_t id;
    int32_t x;
    int32_t y;
};
static_assert(sizeof(KeyboardEvent) == 8, "KeyboardEvent size");
static_assert(sizeof(TouchpadButtonEvent) == 16, "TouchpadButtonEvent size");
static_assert(sizeof(TouchpadMotionEvent) == 12, "TouchpadMotionEvent size");

struct SoundEndSampleInfo {
    int32_t channel;
    int32_t reps_remaining;
    GuestAddr new_data;
    uint32_t num_samples;
};
static_assert(sizeof(SoundEndSampleInfo) == 16, "SoundEndSampleInfo size");

struct SoundGenAudioInfo {
    int32_t channel;
    GuestAddr target;
    uint32_t num_samples;
    int32_t mix;
    GuestAddr orig_start;
    uint32_t orig_num_samples;
    int32_t reps_remaining;
    uint32_t end_sample;
};
static_assert(sizeof(SoundGenAudioInfo) == 32, "SoundGenAudioInfo size");

struct AudioUnitBuffer {
    uint32_t sample_count;
    GuestAddr samples;
    uint32_t sample_rate;
};
static_assert(offsetof(AudioUnitBuffer, samples) == 4, "AudioUnitBuffer samples offset");
static_assert(offsetof(AudioUnitBuffer, sample_rate) == 8, "AudioUnitBuffer rate offset");
static_assert(sizeof(AudioUnitBuffer) == 12, "AudioUnitBuffer size");

} // namespace codboz::guest
