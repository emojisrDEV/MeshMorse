// MeshMorse — Morse code input driver
// Phases 1–4: navigation, text compose, commands, /SM /SC messaging
//
// Timing model (ITU/PARIS):
//   dot_ms      = 1200 / wpm
//   dash_ms     = dashRatio * dot_ms       (ratio 2 or 3, default 3)
//   threshold   = (1 + dashRatio) / 2 * dot_ms
//   letter_gap  = 3 * dot_ms
//   word_gap    = 7 * dot_ms
//
// One-button operation (GPIO 0, active-low):
//   NAV mode    :  .  = next   ..  = prev   -  = select (→ TEXT)   --  = back
//   TEXT mode   :  Morse A-Z 0-9 decode, word-gap = auto-send, ..-- = backspace
//   COMMAND mode:  entered via 2s long hold, word-gap executes "/cmd"

#include "MorseInput.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "modules/MorseScreen.h"
#include <Preferences.h>

MorseInput *morseInput = nullptr;

// ---------------------------------------------------------------------------
// Morse decode table (international, uppercase output)
// Pattern "..--" (Ü) is handled separately as backspace — NOT in this table.
// ---------------------------------------------------------------------------
struct MorseEntry {
    const char *pattern;
    char        ch;
};

static const MorseEntry MORSE_TABLE[] = {
    // Letters
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},
    {".", 'E'},     {"..-.", 'F'},  {"--.", 'G'},    {"....", 'H'},
    {"..", 'I'},    {".---", 'J'},  {"-.-", 'K'},   {".-..", 'L'},
    {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},    {".--.", 'P'},
    {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},    {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'}, {".--", 'W'},    {"-..-", 'X'},
    {"-.--", 'Y'},  {"--..", 'Z'},
    // Digits
    {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
    {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'},
    {"----.", '9'}, {"-----", '0'},
    // Common punctuation
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {".----.", '\''},
    {"-.-.--", '!'}, {"-..-.", '/'},  {"---...", ':'},
    {"-.-.-.", ';'}, {"-.--.", '('},  {"-.--.-", ')'},
    {"-...-", '='},  {"-....-", '-'}, {".--.-.", '@'},
    {nullptr, 0}
};

char MorseInput::decodeMorse(const String &pattern)
{
    for (int i = 0; MORSE_TABLE[i].pattern != nullptr; i++) {
        if (pattern == MORSE_TABLE[i].pattern)
            return MORSE_TABLE[i].ch;
    }
    return '?';
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
MorseInput::MorseInput() : concurrency::OSThread("MorseInput")
{
    loadSettings();
    pinMode(MORSE_KEY_PIN, INPUT_PULLUP);

    // Register as an InputBroker source so the existing Meshtastic UI
    // receives our navigation and keyboard events transparently.
    if (inputBroker)
        inputBroker->registerSource(this);

    LOG_DEBUG("MorseInput init: WPM=%d dash=%d key=GPIO%d", wpm, dashRatio, MORSE_KEY_PIN);
}

// ---------------------------------------------------------------------------
// Main polling loop — called every 5 ms by the OSThread scheduler
// ---------------------------------------------------------------------------
int32_t MorseInput::runOnce()
{
    bool pinHigh  = (digitalRead(MORSE_KEY_PIN) == HIGH);  // HIGH = released
    bool pressed  = !pinHigh;
    bool wasDown  = !lastPinHigh;
    uint32_t now  = millis();

    if (pressed && !wasDown) {
        // ---- Key DOWN ----
        pressStartMs   = now;
        longHoldFired  = false;
        releaseMs      = 0;
        letterGapFired = false;
        wordGapFired   = false;

    } else if (!pressed && wasDown) {
        // ---- Key UP ----
        uint32_t dur = now - pressStartMs;
        if (!longHoldFired && dur >= MORSE_DEBOUNCE_MS) {
            onKeyUp(dur);
        }
        releaseMs = now;

    } else if (pressed && wasDown) {
        // ---- Held down — check for long hold ----
        if (!longHoldFired && (now - pressStartMs) >= MORSE_LONG_HOLD_MS) {
            onLongHold();
            longHoldFired = true;
        }

    } else if (!pressed && !wasDown && releaseMs > 0) {
        // ---- Idle after release — check letter/word gaps ----
        pollGaps();
    }

    lastPinHigh = pinHigh;
    return 5;  // Poll every 5 ms
}

// ---------------------------------------------------------------------------
// Key released — classify as dot or dash
// ---------------------------------------------------------------------------
void MorseInput::onKeyUp(uint32_t durationMs)
{
    if (durationMs >= dashThresholdMs()) {
        currentMorse += '-';
    } else {
        currentMorse += '.';
    }
}

// ---------------------------------------------------------------------------
// Check letter and word gaps while key is idle
// ---------------------------------------------------------------------------
void MorseInput::pollGaps()
{
    if (releaseMs == 0) return;
    uint32_t gap = millis() - releaseMs;

    if (!wordGapFired && gap >= wordGapMs()) {
        onWordGap();
        wordGapFired = true;

    } else if (!letterGapFired && gap >= letterGapMs()) {
        if (!currentMorse.isEmpty()) {
            onLetterGap();
            letterGapFired = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Letter gap fired — decode currentMorse and act on it
// ---------------------------------------------------------------------------
void MorseInput::onLetterGap()
{
    if (currentMorse.isEmpty()) return;

    // Special case: ..-- (Ü pattern) = backspace in TEXT / COMMAND mode
    if (currentMorse == "..--") {
        if (mode == MorseMode::TEXT || mode == MorseMode::COMMAND) {
            doBackspace();
        }
        currentMorse   = "";
        releaseMs      = millis();
        letterGapFired = false;
        wordGapFired   = false;
        return;
    }

    char c = decodeMorse(currentMorse);
    currentMorse = "";

    // Reset gap timer from letter decode time so the word gap gives the user
    // a full wordGapMs() after *each* letter, not just after the last button release.
    releaseMs      = millis();
    letterGapFired = false;
    wordGapFired   = false;

    if (c == '?') return;  // Unknown pattern — ignore silently

    if (mode == MorseMode::NAV) {
        // NAV: route to InputBroker navigation events
        // Build a temporary string for the nav pattern check
        // (We already consumed currentMorse, so re-derive from char)
        // For nav we re-derive the original pattern from c:
        // E(.) = next, I(..) = prev, T(-) = select, M(--) = back
        switch (c) {
            case 'E': { // .
                InputEvent e = {};
                e.source = "morse";
                e.inputEvent = INPUT_BROKER_DOWN;
                this->notifyObservers(&e);
                break;
            }
            case 'I': { // ..
                InputEvent e = {};
                e.source = "morse";
                e.inputEvent = INPUT_BROKER_UP;
                this->notifyObservers(&e);
                break;
            }
            case 'T': { // -  → select and enter TEXT mode
                InputEvent e = {};
                e.source = "morse";
                e.inputEvent = INPUT_BROKER_SELECT;
                this->notifyObservers(&e);
                // Switch to TEXT mode so subsequent keypresses go to compose
                mode = MorseMode::TEXT;
                committedText = "";
                currentMorse  = "";
                LOG_DEBUG("MorseInput: entered TEXT mode");
                break;
            }
            case 'M': { // --  → back / cancel
                InputEvent e = {};
                e.source = "morse";
                e.inputEvent = INPUT_BROKER_BACK;
                this->notifyObservers(&e);
                break;
            }
            default:
                break;  // Any other pattern in NAV mode is ignored
        }

    } else {
        // TEXT or COMMAND mode: append decoded character and forward to InputBroker
        committedText += c;
        textChar(c);
    }
}

// ---------------------------------------------------------------------------
// Word gap fired — auto-send (TEXT) or execute command (COMMAND)
// ---------------------------------------------------------------------------
void MorseInput::onWordGap()
{
    // Commit any partial letter first
    if (!currentMorse.isEmpty()) {
        onLetterGap();
    }

    if (mode == MorseMode::NAV) {
        // Nothing to do in nav mode — just reset tracking
        committedText = "";

    } else if (mode == MorseMode::TEXT) {
        if (!committedText.isEmpty()) {
            sendComposedMessage();
        }

    } else if (mode == MorseMode::COMMAND) {
        if (!committedText.isEmpty()) {
            String cmd = "/" + committedText;
            cmd.toLowerCase();
            executeCommand(cmd);
        }
        committedText = "";
        currentMorse  = "";
        mode = modeBeforeCmd;
        LOG_DEBUG("MorseInput: returned to mode %d", (int)mode);
    }
}

// ---------------------------------------------------------------------------
// Long hold — enter command mode from wherever we are
// ---------------------------------------------------------------------------
void MorseInput::onLongHold()
{
    modeBeforeCmd = mode;
    mode          = MorseMode::COMMAND;
    committedText = "";
    currentMorse  = "";
    LOG_DEBUG("MorseInput: entered COMMAND mode (was %d)", (int)modeBeforeCmd);
    // TODO: screen->showOverlayBanner("CMD", 500) when API is confirmed
}

// ---------------------------------------------------------------------------
// Emit a keyboard character to InputBroker (TEXT / COMMAND mode)
// ---------------------------------------------------------------------------
void MorseInput::textChar(char c)
{
    InputEvent e = {};
    e.source     = "morse";
    e.inputEvent = INPUT_BROKER_ANYKEY;
    e.kbchar     = c;
    this->notifyObservers(&e);
}

// ---------------------------------------------------------------------------
// Backspace — remove last char from local buffer and emit to InputBroker
// ---------------------------------------------------------------------------
void MorseInput::doBackspace()
{
    if (!committedText.isEmpty())
        committedText.remove(committedText.length() - 1);

    InputEvent e = {};
    e.source     = "morse";
    e.inputEvent = INPUT_BROKER_BACK;
    e.kbchar     = 0x08;
    this->notifyObservers(&e);
}

// ---------------------------------------------------------------------------
// Send the composed message on the primary channel, then reset to NAV
// ---------------------------------------------------------------------------
void MorseInput::sendComposedMessage()
{
    sendToChannel(channels.getPrimaryIndex(), committedText);
    committedText = "";
    currentMorse  = "";
    mode          = MorseMode::NAV;
    LOG_DEBUG("MorseInput: message sent, returned to NAV");
}

// ---------------------------------------------------------------------------
// Command execution — called with lowercase cmd starting with '/'
//
// Supported commands:
//   /wpm##          — set WPM (5–50)
//   /wpm            — log current WPM (screen notification TODO)
//   /dash2 /dash3   — set dash ratio
//   /dt /dm /dh     — set display mode
//   /b              — cancel compose, go back
//   /sc# msg        — send msg to Meshtastic channel 0–7
//   /sm name msg    — send DM to node with matching short name
// ---------------------------------------------------------------------------
void MorseInput::executeCommand(const String &cmd)
{
    if (!cmd.startsWith("/")) return;
    String body = cmd.substring(1);  // Strip leading '/'

    // /wpm## or /wpm ##
    if (body.startsWith("wpm")) {
        String numPart = body.substring(3);
        numPart.trim();
        if (numPart.length() == 0) {
            LOG_DEBUG("MorseInput: WPM=%d dash=%d", wpm, dashRatio);
            // TODO: screen banner
        } else {
            int v = numPart.toInt();
            if (v >= MORSE_MIN_WPM && v <= MORSE_MAX_WPM) {
                wpm = (uint8_t)v;
                saveSettings();
                LOG_DEBUG("MorseInput: WPM set to %d (dot=%dms)", wpm, dotMs());
            }
        }
        return;
    }

    // /dash2 or /dash3
    if (body == "dash2") {
        dashRatio = 2; saveSettings();
        LOG_DEBUG("MorseInput: dash ratio = 2");
        return;
    }
    if (body == "dash3") {
        dashRatio = 3; saveSettings();
        LOG_DEBUG("MorseInput: dash ratio = 3");
        return;
    }

    // /dt /dm /dh
    if (body == "dt") {
        displayMode = MorseDisplayMode::TEXT;  saveSettings();
        LOG_DEBUG("MorseInput: display = TEXT");  return;
    }
    if (body == "dm") {
        displayMode = MorseDisplayMode::MORSE; saveSettings();
        LOG_DEBUG("MorseInput: display = MORSE"); return;
    }
    if (body == "dh") {
        displayMode = MorseDisplayMode::HYBRID; saveSettings();
        LOG_DEBUG("MorseInput: display = HYBRID"); return;
    }

    // /b — cancel compose and go back to nav
    if (body == "b") {
        committedText = "";
        currentMorse  = "";
        mode          = MorseMode::NAV;
        InputEvent e  = {};
        e.source      = "morse";
        e.inputEvent  = INPUT_BROKER_BACK;
        this->notifyObservers(&e);
        LOG_DEBUG("MorseInput: /b — cancelled, returned to NAV");
        return;
    }

    // /sc# message  — send to channel 0–7
    // Body format: "sc0hello world" or "sc0 hello world"
    if (body.length() >= 3 && body.startsWith("sc")) {
        char chChar = body[2];
        if (chChar >= '0' && chChar <= '7') {
            uint8_t ch  = (uint8_t)(chChar - '0');
            String  msg = body.substring(3);
            msg.trim();
            if (!msg.isEmpty()) {
                sendToChannel(ch, msg);
                LOG_DEBUG("MorseInput: /sc%d sent: %s", ch, msg.c_str());
            }
        }
        return;
    }

    // /sm shortname message  — send DM to named node
    // Body format: "sm n2abc hello world"
    if (body.startsWith("sm ")) {
        String rest = body.substring(3);
        rest.trim();
        int spaceIdx = rest.indexOf(' ');
        if (spaceIdx > 0) {
            String name = rest.substring(0, spaceIdx);
            String msg  = rest.substring(spaceIdx + 1);
            msg.trim();
            if (!msg.isEmpty()) {
                sendToDM(name, msg);
                LOG_DEBUG("MorseInput: /sm %s: %s", name.c_str(), msg.c_str());
            }
        }
        return;
    }

    LOG_DEBUG("MorseInput: unknown command: %s", cmd.c_str());
}

// ---------------------------------------------------------------------------
// Send a text message to a specific Meshtastic channel (0–7)
// ---------------------------------------------------------------------------
void MorseInput::sendToChannel(uint8_t channelIdx, const String &msg)
{
    if (!router || !service) return;

    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) return;

    p->to                     = NODENUM_BROADCAST;
    p->channel                = channelIdx;
    p->decoded.portnum        = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->want_ack               = false;
    p->decoded.has_bitfield   = false;

    size_t len = msg.length();
    if (len > sizeof(p->decoded.payload.bytes) - 1)
        len = sizeof(p->decoded.payload.bytes) - 1;
    memcpy(p->decoded.payload.bytes, msg.c_str(), len);
    p->decoded.payload.size = len;

    service->sendToMesh(p, RX_SRC_LOCAL);

    // Show our own outgoing message in the chat history
    if (morseScreen)
        morseScreen->addOutgoing(msg);
}

// ---------------------------------------------------------------------------
// Send a direct message to a node identified by short name (case-insensitive)
// ---------------------------------------------------------------------------
void MorseInput::sendToDM(const String &shortName, const String &msg)
{
    if (!router || !service || !nodeDB) return;

    // Search nodeDB for a node whose short_name matches (case-insensitive)
    NodeNum targetNum = 0;
    String  target    = shortName;
    target.toLowerCase();

    for (int i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node || !node->has_user) continue;
        String sn = String(node->user.short_name);
        sn.toLowerCase();
        if (sn == target) {
            targetNum = node->num;
            break;
        }
    }

    if (targetNum == 0) {
        LOG_DEBUG("MorseInput: /sm — node '%s' not found in nodeDB", shortName.c_str());
        return;
    }

    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) return;

    p->to                   = targetNum;
    p->channel              = channels.getPrimaryIndex();
    p->decoded.portnum      = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->want_ack             = false;
    p->decoded.has_bitfield = false;

    size_t len = msg.length();
    if (len > sizeof(p->decoded.payload.bytes) - 1)
        len = sizeof(p->decoded.payload.bytes) - 1;
    memcpy(p->decoded.payload.bytes, msg.c_str(), len);
    p->decoded.payload.size = len;

    service->sendToMesh(p, RX_SRC_LOCAL);
}

// ---------------------------------------------------------------------------
// NVS persistence — "morse" namespace
// ---------------------------------------------------------------------------
void MorseInput::loadSettings()
{
    Preferences prefs;
    prefs.begin("morse", /*readOnly=*/true);
    wpm         = prefs.getUChar("wpm",  MORSE_DEFAULT_WPM);
    dashRatio   = prefs.getUChar("dash", MORSE_DEFAULT_DASH_RATIO);
    displayMode = (MorseDisplayMode)prefs.getUChar("disp", (uint8_t)MorseDisplayMode::HYBRID);
    prefs.end();

    // Clamp to valid ranges
    if (wpm < MORSE_MIN_WPM) wpm = MORSE_MIN_WPM;
    if (wpm > MORSE_MAX_WPM) wpm = MORSE_MAX_WPM;
    if (dashRatio < 2 || dashRatio > 3) dashRatio = MORSE_DEFAULT_DASH_RATIO;
    if ((uint8_t)displayMode > (uint8_t)MorseDisplayMode::HYBRID)
        displayMode = MorseDisplayMode::HYBRID;
}

void MorseInput::saveSettings()
{
    Preferences prefs;
    prefs.begin("morse", /*readOnly=*/false);
    prefs.putUChar("wpm",  wpm);
    prefs.putUChar("dash", dashRatio);
    prefs.putUChar("disp", (uint8_t)displayMode);
    prefs.end();
}
