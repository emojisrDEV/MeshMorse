#include "meshtastic.h"
#include <Arduino.h>
#include <Wire.h>
#include "HT_SSD1306Wire.h"
#define BUTTON_PIN 0
#define BUZZER_PIN 25
#define DOT_THRESHOLD 200
#define LETTER_GAP 800
#define WORD_GAP 2000

SSD1306Wire display(0x3c, SDA, SCL);

bool lastState = HIGH;
unsigned long pressStart = 0;
unsigned long lastInput = 0;

String morseBuffer = "";
String wordBuffer = "";

String decodeMorse(String code) {
  if (code==".-") return "A"; if (code=="-...") return "B";
  if (code=="-.-.") return "C"; if (code=="-..") return "D";
  if (code==".") return "E"; if (code=="..-.") return "F";
  if (code=="--.") return "G"; if (code=="....") return "H";
  if (code=="..") return "I"; if (code==".---") return "J";
  if (code=="-.-") return "K"; if (code==".-..") return "L";
  if (code=="--") return "M"; if (code=="-.") return "N";
  if (code=="---") return "O"; if (code==".--.") return "P";
  if (code=="--.-") return "Q"; if (code==".-.") return "R";
  if (code=="...") return "S"; if (code=="-") return "T";
  if (code=="..-") return "U"; if (code=="...-") return "V";
  if (code==".--") return "W"; if (code=="-..-") return "X";
  if (code=="-.--") return "Y"; if (code=="--..") return "Z";

  // Numbers
  if (code==".----") return "1"; if (code=="..---") return "2";
  if (code=="...--") return "3"; if (code=="....-") return "4";
  if (code==".....") return "5"; if (code=="-....") return "6";
  if (code=="--...") return "7"; if (code=="---..") return "8";
  if (code=="----.") return "9"; if (code=="-----") return "0";

  return "?";
}

void updateDisplay(String line1, String line2) {
  display.clear();
  display.drawString(0, 0, "TX:");
  display.drawString(0, 10, line1);
  display.drawString(0, 30, "RX:");
  display.drawString(0, 40, line2);
  display.display();
}

void sendWord(String word) {
  if (word.length() == 0) return;

  Serial.print("Sending: ");
  Serial.println(word);

  service.sendText(word.c_str());

  updateDisplay(word, "");
}

class MorseRx : public MeshService {
public:
  void onReceive(const MeshPacket &p) override {
    String msg = String((char*)p.payload.bytes);

    Serial.print("RX: ");
    Serial.println(msg);

    updateDisplay("", msg);
  }
};

MorseRx morseRx;

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  Serial.begin(115200);

  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  service.addObserver(&morseRx);

  updateDisplay("READY", "");
}

void loop() {
  bool state = digitalRead(BUTTON_PIN);


  if (state == LOW && lastState == HIGH) {
    pressStart = millis();
    tone(BUZZER_PIN, 800); 
  }

  if (state == HIGH && lastState == LOW) {
    noTone(BUZZER_PIN);

    unsigned long duration = millis() - pressStart;

    if (duration < DOT_THRESHOLD) {
      morseBuffer += ".";
      Serial.print(".");
    } else {
      morseBuffer += "-";
      Serial.print("-");
    }

    lastInput = millis();
  }

  if (morseBuffer.length() > 0 && millis() - lastInput > LETTER_GAP) {
    String letter = decodeMorse(morseBuffer);

    Serial.print(" -> ");
    Serial.println(letter);

    wordBuffer += letter;
    morseBuffer = "";

    updateDisplay(wordBuffer, "");
  }

  if (wordBuffer.length() > 0 && millis() - lastInput > WORD_GAP) {
    sendWord(wordBuffer);
    wordBuffer = "";
  }

  lastState = state;
}