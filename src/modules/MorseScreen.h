#pragma once
#ifdef MORSE_INPUT_ENABLED

#include "SinglePortModule.h"
#include "input/MorseInput.h"

// MorseScreen — UI frame and incoming-message tracker for MeshMorse.
//
// Always shows as a frame in the Meshtastic screen carousel.
// Displays:
//   Title bar  — mode (NAV/MSG/CMD), WPM, dash ratio, display mode
//   Lines 1-2  — last two incoming text messages
//   Line 3     — compose line (dots/dashes + decoded text in real time)
//
// Also receives TEXT_MESSAGE_APP packets to populate the message history.

class MorseScreen : public SinglePortModule
{
  public:
    MorseScreen();

    bool wantUIFrame() override { return true; }

#if HAS_SCREEN
    void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state,
                   int16_t x, int16_t y) override;
#endif

    // Called by MorseInput when it sends a message so we can show it too
    void addOutgoing(const String &text);

  protected:
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    // Circular buffer of last 3 messages (incoming + outgoing)
    static const int HISTORY_SIZE = 3;
    struct ChatMsg {
        char from[6];   // short name (4 chars) + null, or "me"
        char text[22];  // truncated message text + null
    } history[HISTORY_SIZE];
    int histHead  = 0;  // next write slot
    int histCount = 0;  // how many valid entries (0–HISTORY_SIZE)

    void addToHistory(const char *from, const char *text);
    String buildComposeLine() const;
};

extern MorseScreen *morseScreen;

#endif // MORSE_INPUT_ENABLED
