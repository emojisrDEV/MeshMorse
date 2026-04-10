#pragma once

// MeshMorse — Morse code input driver for Meshtastic (Heltec V3)
// Phases 1–4: nav, text compose, commands, /SM /SC messaging
//
// Compile with -DMORSE_INPUT_ENABLED to activate.
// Uses MORSE_KEY_PIN (default BUTTON_PIN = GPIO 0).
// Disables the standard ButtonThread so it owns the GPIO.

#include "InputBroker.h"
#include "Observer.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include <Arduino.h>

// --- Hardware defaults ---
#ifndef MORSE_KEY_PIN
#define MORSE_KEY_PIN BUTTON_PIN  // GPIO 0 on Heltec V3
#endif

// --- Timing ---
#define MORSE_DEFAULT_WPM       15
#define MORSE_DEFAULT_DASH_RATIO 3
#define MORSE_MIN_WPM           5
#define MORSE_MAX_WPM           50
#define MORSE_LONG_HOLD_MS      2000   // Hold to enter command mode
#define MORSE_DEBOUNCE_MS       15     // Ignore presses shorter than this

// --- Modes ---
enum class MorseMode {
    NAV,      // Navigate Meshtastic menus
    TEXT,     // Compose a message character by character
    COMMAND   // Slash-command entry (entered via long hold)
};

enum class MorseDisplayMode {
    TEXT,     // /dt — decoded letters only
    MORSE,    // /dm — raw dots/dashes only
    HYBRID    // /dh — letters + active Morse pattern (default)
};

class MorseInput : public Observable<const InputEvent *>, public concurrency::OSThread
{
  public:
    explicit MorseInput();

    // --- Persisted settings ---
    uint8_t wpm        = MORSE_DEFAULT_WPM;
    uint8_t dashRatio  = MORSE_DEFAULT_DASH_RATIO;
    MorseDisplayMode displayMode = MorseDisplayMode::HYBRID;

    // --- Compose state (readable by display code) ---
    String committedText;  // Fully decoded letters so far
    String currentMorse;   // Dots/dashes of letter currently being keyed

    // --- Current mode ---
    MorseMode mode = MorseMode::NAV;

    // --- Computed timing (inline, called often) ---
    inline uint32_t dotMs()           const { return 1200 / wpm; }
    inline uint32_t dashThresholdMs() const { return dotMs() * (1 + dashRatio) / 2; }
    inline uint32_t letterGapMs()     const { return dotMs() * 3; }
    inline uint32_t wordGapMs()       const { return dotMs() * 7; }

  protected:
    int32_t runOnce() override;

  private:
    // --- GPIO state ---
    bool     lastPinHigh     = true;   // HIGH = not pressed (active-low)
    uint32_t pressStartMs    = 0;
    uint32_t releaseMs       = 0;
    bool     longHoldFired   = false;
    bool     letterGapFired  = false;
    bool     wordGapFired    = false;
    MorseMode modeBeforeCmd  = MorseMode::NAV;

    // --- State machine ---
    void onKeyDown();
    void onKeyUp(uint32_t durationMs);
    void pollGaps();
    void onLetterGap();
    void onWordGap();
    void onLongHold();

    // --- Decode & dispatch ---
    static char decodeMorse(const String &pattern);
    void navFromPattern(const String &pattern);
    void textChar(char c);
    void doBackspace();
    void sendComposedMessage();

    // --- Command execution ---
    void executeCommand(const String &cmd);  // cmd must start with '/'
    void sendToChannel(uint8_t channelIdx, const String &msg);
    void sendToDM(const String &shortName, const String &msg);

    // --- NVS persistence ---
    void loadSettings();
    void saveSettings();
};

extern MorseInput *morseInput;
