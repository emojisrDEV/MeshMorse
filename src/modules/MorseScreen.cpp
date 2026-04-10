// MorseScreen — UI frame for MeshMorse
// Reads live state from morseInput global (MorseInput.h).
// Receives incoming text messages to populate chat history.

#include "MorseScreen.h"

#ifdef MORSE_INPUT_ENABLED

#include "graphics/Screen.h"
#include "mesh/NodeDB.h"

MorseScreen *morseScreen = nullptr;

MorseScreen::MorseScreen()
    : SinglePortModule("MorseScreen", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
}

// ---------------------------------------------------------------------------
// Receive incoming text messages — add to history ring buffer
// ---------------------------------------------------------------------------
ProcessMessage MorseScreen::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Ignore our own transmissions (already added via addOutgoing)
    if (mp.from == nodeDB->getNodeNum())
        return ProcessMessage::CONTINUE;

    // Resolve sender short name
    char fromName[6] = "????";
    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(mp.from);
    if (node && node->has_user && node->user.short_name[0]) {
        strncpy(fromName, node->user.short_name, 4);
        fromName[4] = '\0';
    } else {
        snprintf(fromName, sizeof(fromName), "%04X", (unsigned)(mp.from & 0xFFFF));
    }

    // Extract message text
    char text[22] = {};
    size_t len = mp.decoded.payload.size;
    if (len > 21) len = 21;
    memcpy(text, mp.decoded.payload.bytes, len);
    text[len] = '\0';

    addToHistory(fromName, text);
    return ProcessMessage::CONTINUE;  // let TextMessageModule process it too
}

// ---------------------------------------------------------------------------
// Called by MorseInput after a message is sent
// ---------------------------------------------------------------------------
void MorseScreen::addOutgoing(const String &text)
{
    char buf[22];
    strncpy(buf, text.c_str(), 21);
    buf[21] = '\0';
    addToHistory("me", buf);
}

// ---------------------------------------------------------------------------
// Ring buffer helpers
// ---------------------------------------------------------------------------
void MorseScreen::addToHistory(const char *from, const char *text)
{
    strncpy(history[histHead].from, from, 5);
    history[histHead].from[5] = '\0';
    strncpy(history[histHead].text, text, 21);
    history[histHead].text[21] = '\0';
    histHead = (histHead + 1) % HISTORY_SIZE;
    if (histCount < HISTORY_SIZE) histCount++;
}

// ---------------------------------------------------------------------------
// Build the compose line for the bottom row
// ---------------------------------------------------------------------------
String MorseScreen::buildComposeLine() const
{
    if (!morseInput) return "> --";

    String content;
    switch (morseInput->mode) {
        case MorseMode::NAV:
            return ">  [ - to compose ]";

        case MorseMode::COMMAND:
            content = "CMD:" + morseInput->committedText + morseInput->currentMorse;
            break;

        case MorseMode::TEXT:
            switch (morseInput->displayMode) {
                case MorseDisplayMode::MORSE:
                    // Show committed text + current morse pattern
                    content = morseInput->committedText + " " + morseInput->currentMorse;
                    break;
                case MorseDisplayMode::TEXT:
                    content = morseInput->committedText;
                    break;
                case MorseDisplayMode::HYBRID:
                default:
                    // Letters left, active morse right — clears each letter gap
                    content = morseInput->committedText + morseInput->currentMorse;
                    break;
            }
            break;
    }

    String line = "> " + content;

    // Show the tail end of long messages so current typing is always visible
    const int maxLen = 20;
    if ((int)line.length() > maxLen)
        line = "~" + line.substring(line.length() - (maxLen - 1));

    return line;
}

// ---------------------------------------------------------------------------
// Draw the frame — called by Screen on every display refresh
// ---------------------------------------------------------------------------
#if HAS_SCREEN
void MorseScreen::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state,
                             int16_t x, int16_t y)
{
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // --- Title bar (inverted) ---
    char title[28];
    const char *modeStr = "NAV";
    const char *dispStr = "DH";
    if (morseInput) {
        if      (morseInput->mode == MorseMode::TEXT)    modeStr = "MSG";
        else if (morseInput->mode == MorseMode::COMMAND) modeStr = "CMD";
        if      (morseInput->displayMode == MorseDisplayMode::TEXT)  dispStr = "DT";
        else if (morseInput->displayMode == MorseDisplayMode::MORSE) dispStr = "DM";
        snprintf(title, sizeof(title), "MORSE %s W:%d D:%d %s",
                 modeStr,
                 morseInput->wpm,
                 morseInput->dashRatio,
                 dispStr);
    } else {
        snprintf(title, sizeof(title), "MORSE (init)");
    }

    if (config.display.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_INVERTED)
        display->fillRect(x, y, display->getWidth(), FONT_HEIGHT_SMALL);
    display->setColor(OLEDDISPLAY_COLOR::BLACK);
    display->drawString(x, y, title);
    display->setColor(OLEDDISPLAY_COLOR::WHITE);

    // --- Message history (last 2 entries) ---
    // Walk ring buffer oldest→newest, show last 2
    int shown = 0;
    for (int i = 0; i < histCount && shown < 2; i++) {
        // Map to slot: oldest slot = histHead when buffer is full,
        //              or slot 0 when not yet full
        int slot;
        if (histCount < HISTORY_SIZE) {
            slot = i;
        } else {
            slot = (histHead + i) % HISTORY_SIZE;
        }
        // Only show the last 2 (skip older ones if histCount == 3)
        if (histCount == HISTORY_SIZE && i == 0) continue;

        char line[28];
        snprintf(line, sizeof(line), "<%s> %s", history[slot].from, history[slot].text);
        // Clip to screen width — drawString clips automatically
        display->drawString(x, y + FONT_HEIGHT_SMALL * (shown + 1), line);
        shown++;
    }

    // --- Compose line ---
    String composeLine = buildComposeLine();
    display->drawString(x, y + FONT_HEIGHT_SMALL * 3, composeLine.c_str());
}
#endif // HAS_SCREEN

#endif // MORSE_INPUT_ENABLED
