#define BUTTON_PIN 0  

#define DASH_TIME 500
#define LETTER_GAP 800
#define WORD_GAP 1500

String currentMorse = "";
String decodedText = "";

unsigned long pressStart = 0;
unsigned long lastInputTime = 0;

bool lastState = HIGH;

struct MorseMap {
  const char* code;
  char letter;
};

MorseMap morseTable[] = {
  {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'},
  {"..-.", 'F'}, {"--.", 'G'}, {"....", 'H'}, {"..", 'I'}, {".---", 'J'},
  {"-.-", 'K'}, {".-..", 'L'}, {"--", 'M'}, {"-.", 'N'}, {"---", 'O'},
  {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'},
  {"..-", 'U'}, {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'}, {"-.--", 'Y'},
  {"--..", 'Z'}
};

char decodeMorse(String code) {
  for (auto &m : morseTable) {
    if (code == m.code) return m.letter;
  }
  return '?';
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.println("Morse Ready");
  Serial.println("Tap button...");
}

void loop() {
  bool currentState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (lastState == HIGH && currentState == LOW) {
    pressStart = now;
  }

  if (lastState == LOW && currentState == HIGH) {
    unsigned long pressDuration = now - pressStart;

    if (pressDuration < DASH_TIME) {
      currentMorse += ".";
      Serial.print(".");
    } else {
      currentMorse += "-";
      Serial.print("-");
    }

    lastInputTime = now;
  }

  if (currentMorse.length() > 0 && (now - lastInputTime) > LETTER_GAP) {
    char letter = decodeMorse(currentMorse);
    decodedText += letter;

    Serial.print(" -> ");
    Serial.println(letter);

    currentMorse = "";
  }

  if ((now - lastInputTime) > WORD_GAP && !decodedText.endsWith(" ")) {
    decodedText += " ";
    Serial.println(" (space)");
  }

  lastState = currentState;
}