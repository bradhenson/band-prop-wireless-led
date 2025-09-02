/**
 * @file pico_receiver_radiohead.ino
 * @brief Receiver firmware converted to use the RadioHead library.
 * @version 8.4
 * @date 2025-09-01
 *
 * V8.4 Changes:
 * - Added a "Setup Mode" title to the OLED screen during ID selection for
 * improved user clarity.
 *
 * V8.3 Changes:
 * - Rewrote button handling in Setup Mode to be non-blocking, ensuring the
 * OLED display updates instantly with each button press.
 * - Simplified the Setup Mode UI to display only the large ID number for clarity.
 *
 * V8.2 Changes:
 * - Corrected the button handling logic within Setup Mode.
 *
 * V8.1 Changes:
 * - Restored the full "Setup Mode" functionality.
 *
 * V8.0 Changes:
 * - Ported all radio functions from RadioLib to RadioHead (RH_RF69).
 */

// ============================ LIBRARIES ============================
#include <SPI.h>
#include <RH_RF69.h> // Using RadioHead library
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include "hardware/watchdog.h" // Required for rebooting

// ====================== HARDWARE & SYSTEM CONFIG ===================
#define LED_PIN           22
#define NUM_LEDS          120
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_RGB + NEO_KHZ800);

#define OLED_SDA_PIN      10
#define OLED_SCL_PIN      11
Adafruit_SSD1306 display(128, 32, &Wire1, -1);

#define CONFIG_BUTTON_PIN 28

// --- RFM69 Radio (RadioHead Configuration) ---
#define RF69_FREQ         915.0
#define RF69_CS_PIN       17
#define RF69_INT_PIN      21
#define RF69_RST_PIN      20
const char ENCRYPTKEY[] = "GoMarchingBand!!";

RH_RF69 driver(RF69_CS_PIN, RF69_INT_PIN);

// =========================== DATA & STATE ==========================
// This struct MUST exactly match the one in the remote's code.
struct RadioPacket {
    uint32_t packetCounter;
    uint8_t  sequenceId;
};

uint8_t propID = 1;
bool inTestMode = false;
bool inSetupMode = false;
bool inDiagnosticMode = false;
uint8_t currentSequence = 0;

volatile bool buttonPressFlag = false;
volatile unsigned long buttonPressTime = 0;

unsigned long lastPacketTime = 0;
#define PACKET_TIMEOUT_MS 3000
#define STRIP_TIMEOUT_MS 1800000UL // 30 minutes
unsigned long lastActiveAnimationTime = 0;

// --- Animation & Display State ---
uint16_t animationStep = 0;
unsigned long lastAnimationTime = 0;
unsigned long lastDisplayUpdateTime = 0;
int16_t lastRSSI = 0;

// --- Frame change tracking ---
bool frameDirty = false;
bool stripIsOff = true;
static inline void markDirty()   { frameDirty = true; stripIsOff = false; }
static inline void markAllOff()  { stripIsOff = true; frameDirty = true;  }
static inline void showIfDirty() { if (frameDirty) { strip.show(); frameDirty = false; } }

// ====================== INTERRUPT SERVICE (button) =================
void button_isr() {
    static unsigned long last_interrupt_time = 0;
    unsigned long interrupt_time = millis();
    if (interrupt_time - last_interrupt_time > 200) {
        buttonPressFlag = true;
        buttonPressTime = interrupt_time;
    }
    last_interrupt_time = interrupt_time;
}

// ============================== UTILS ==============================
void savePropID(uint8_t id) { EEPROM.write(0, id); EEPROM.commit(); }
uint8_t loadPropID() {
    uint8_t id = EEPROM.read(0);
    if (id < 1 || id > 18) { id = 1; savePropID(id); }
    return id;
}

// ============================ ANIMATIONS ===========================
// (Animation functions: animationOff, fadeAll, animationRainbow, etc. are unchanged)
void animationOff() {
    if (!stripIsOff) { strip.clear(); markAllOff(); }
}

void fadeAll(uint8_t amount) {
    uint16_t n = strip.numPixels();
    for (uint16_t i = 0; i < n; i++) {
        uint32_t c = strip.getPixelColor(i);
        uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        r = (uint16_t(r) * (255 - amount)) >> 8;
        g = (uint16_t(g) * (255 - amount)) >> 8;
        b = (uint16_t(b) * (255 - amount)) >> 8;
        strip.setPixelColor(i, r, g, b);
    }
    markDirty();
}

void animationRainbow() {
    if (millis() - lastAnimationTime < 16) return;
    lastAnimationTime = millis();
    animationStep++;
    uint16_t n = strip.numPixels();
    for (uint16_t i = 0; i < n; i++) {
        strip.setPixelColor(i, strip.gamma32(strip.ColorHSV((uint32_t)i * 65536 / n + (uint32_t)animationStep * 256)));
    }
    markDirty();
}

void animationChase() {
    if (millis() - lastAnimationTime < 16) return;
    lastAnimationTime = millis();
    fadeAll(200);
    strip.setPixelColor(animationStep, 255, 0, 0);
    animationStep = (animationStep + 1) % strip.numPixels();
    markDirty();
}

void animationWipe() {
    if (millis() - lastAnimationTime < 16) return;
    lastAnimationTime = millis();
    uint16_t n = strip.numPixels();
    if (animationStep < n) {
        strip.setPixelColor(animationStep, 0, 0, 255);
    } else if (animationStep < 2 * n) {
        strip.setPixelColor(animationStep - n, 0);
    }
    animationStep++;
    if (animationStep >= 2 * n) animationStep = 0;
    markDirty();
}

void animationSparkle() {
    if (millis() - lastAnimationTime < 16) return;
    lastAnimationTime = millis();
    fadeAll(100);
    strip.setPixelColor((uint16_t)random(strip.numPixels()), 255, 255, 255);
    markDirty();
}

void animationCylon() {
    if (millis() - lastAnimationTime < 16) return;
    lastAnimationTime = millis();
    static int pos = 0, dir = 1;
    fadeAll(40);
    strip.setPixelColor(pos, 255, 0, 0);
    pos += dir;
    if (pos <= 0 || pos >= (int)strip.numPixels() - 1) dir = -dir;
    markDirty();
}

void runSequence() {
    switch (currentSequence) {
        case 0: animationOff();     break;
        case 1: animationRainbow(); break;
        case 2: animationChase();   break;
        case 3: animationWipe();    break;
        case 4: animationSparkle(); break;
        case 5: animationCylon();   break;
        default: animationOff();    break;
    }
}


// ====================== SYSTEM & UI FUNCTIONS ======================
void updateDisplay() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);

    display.print(F("ID:"));
    display.print(propID);
    display.print(F(" S:"));
    display.print(currentSequence);

    display.setCursor(64, 0);
    display.print(F("RSSI:"));
    display.print(lastRSSI);

    display.setCursor(0, 10);
    if (inDiagnosticMode) {
        display.print(F("MODE: DIAGNOSTIC"));
    } else if (inSetupMode) {
        display.print(F("MODE: SETUP"));
    } else if (inTestMode) {
        display.print(F("MODE: TEST"));
    } else if (millis() - lastPacketTime > PACKET_TIMEOUT_MS) {
        display.print(F("MODE: NO SIGNAL"));
    } else {
        display.print(F("MODE: READY"));
    }
    display.display();
}

void handleSetupMode() {
    uint8_t tempPropID = propID;
    unsigned long pressStartTime = 0;
    bool isPressed = false;
    bool lastIsPressed = false;
    
    while (true) {
        // Always redraw the display at the start of the loop
        display.clearDisplay();
        
        // --- Draw Title ---
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(34, 0); // Centered text: (128 - (10 chars * 6 pixels))/2
        display.print(F("Setup Mode"));

        // --- Draw Large ID ---
        display.setTextSize(3); // Large, clear font
        if (tempPropID < 10) { 
            display.setCursor(56, 9); // Center single digit
        } else { 
            display.setCursor(47, 9); // Center double digit
        }
        display.print(tempPropID);
        display.display();

        isPressed = (digitalRead(CONFIG_BUTTON_PIN) == LOW);

        // Check for the moment the button is first pressed
        if (isPressed && !lastIsPressed) {
            pressStartTime = millis();
        }

        // Check for the moment the button is released
        if (!isPressed && lastIsPressed) {
            // This was a short press (a tap), so cycle the ID
            tempPropID++;
            if (tempPropID > 18) {
                tempPropID = 1;
            }
        }
        
        // Independently check for a long hold
        if (isPressed && (millis() - pressStartTime > 3000)) {
            // Button has been held for 3+ seconds. Save and reboot.
            propID = tempPropID;
            savePropID(propID);
            
            display.clearDisplay();
            display.setTextSize(2);
            display.setCursor(18, 8);
            display.print("SAVED!");
            display.display();

            for (int i = 0; i < 3; i++) {
                strip.fill(strip.Color(0, 255, 0), 0, NUM_LEDS); strip.show(); delay(150);
                strip.clear(); strip.show(); delay(150);
            }
            watchdog_reboot(0, 0, 100);
        }

        lastIsPressed = isPressed; // Remember the state for the next loop
        delay(20); // Polling delay to prevent bouncing and save CPU cycles
    }
}


// =============================== SETUP =============================
void setup() {
    Serial.begin(115200);
    delay(1000);
    EEPROM.begin(256);

    Wire1.setSDA(OLED_SDA_PIN);
    Wire1.setSCL(OLED_SCL_PIN);
    Wire1.begin();
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
    }
    display.setRotation(2);
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0,0);
    display.print("Receiver Boot...");
    display.display();

    strip.begin();
    strip.setBrightness(100);
    strip.clear();
    markAllOff();
    showIfDirty();

    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    delay(100);

    // Load ID first to pass to setup mode
    propID = loadPropID();

    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
        inSetupMode = true;
        handleSetupMode(); // This function will loop forever until ID is saved and rebooted.
    }

    attachInterrupt(digitalPinToInterrupt(CONFIG_BUTTON_PIN), button_isr, FALLING);

    // --- RadioHead Initialization Sequence ---
    pinMode(RF69_RST_PIN, OUTPUT);
    digitalWrite(RF69_RST_PIN, LOW);
    digitalWrite(RF69_RST_PIN, HIGH);
    delay(10);
    digitalWrite(RF69_RST_PIN, LOW);
    delay(10);

    if (!driver.init()) {
        display.clearDisplay(); display.setCursor(5, 8); display.print(F("RADIO FAIL")); display.display();
        while (true);
    }
    if (!driver.setFrequency(RF69_FREQ)) {
        display.clearDisplay(); display.setCursor(5, 8); display.print(F("FREQ FAIL")); display.display();
        while (true);
    }
    driver.setEncryptionKey((uint8_t*)ENCRYPTKEY);
    
    // The receiver doesn't transmit, but setting power is good practice.
    driver.setTxPower(20, true);

    updateDisplay();
}

// ================================ LOOP ============================
void loop() {
    // 1. Handle Radio Input
    if (driver.available()) {
        uint8_t buf[sizeof(RadioPacket)];
        uint8_t len = sizeof(buf);

        if (driver.recv(buf, &len)) {
            lastPacketTime = millis();
            lastRSSI = driver.lastRssi();

            RadioPacket packet;
            memcpy(&packet, buf, sizeof(packet));

            // Only update sequence if it has changed
            if (currentSequence != packet.sequenceId) {
                currentSequence = packet.sequenceId;
                animationStep = 0; // Reset animation on change
                strip.clear();
                markDirty();
                if (currentSequence > 0) {
                    lastActiveAnimationTime = millis();
                }
            }
        }
    }

    // 2. Handle System State & Timeouts
    if (millis() - lastPacketTime > PACKET_TIMEOUT_MS) {
        if (currentSequence != 0) {
            currentSequence = 0;
            animationStep = 0;
        }
    }
    
    if (currentSequence != 0 && (millis() - lastActiveAnimationTime > STRIP_TIMEOUT_MS)) {
        currentSequence = 0;
        animationStep = 0;
    }

    // 3. Handle Button Input
    if (buttonPressFlag) {
        // Long press check
        if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
            if (!inDiagnosticMode && millis() - buttonPressTime >= 5000) {
                inDiagnosticMode = true; inTestMode = false;
                currentSequence = 0;
                buttonPressFlag = false; // Consume flag
            }
        } else { // Button was released
            if (inDiagnosticMode) {
                inDiagnosticMode = false;
            } else {
                inTestMode = !inTestMode;
                currentSequence = 0; // Always turn off LEDs when toggling modes
            }
            animationStep = 0;
            buttonPressFlag = false; // Consume flag
        }
    }

    // 4. Run LEDs
    if (inDiagnosticMode) {
        animationOff();
    } else if (inTestMode) {
        animationRainbow();
    } else {
        runSequence();
    }
    showIfDirty();
    
    // 5. Update Display (throttled to 4Hz)
    if (millis() - lastDisplayUpdateTime > 250) {
        updateDisplay();
        lastDisplayUpdateTime = millis();
    }
}

