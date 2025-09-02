/**
 * @file uno_remote_radiohead.ino
 * @brief Remote control with non-blocking transmission for maximum responsiveness.
 * @version 7.3
 * @date 2025-09-01
 *
 * V7.3 Changes:
 * - Added a dedicated "OFF" button on pin 7.
 * - Restored "Summerville Band" startup message on the LCD.
 *
 * V7.2 Changes:
 * - Corrected the button debouncing logic to reliably detect every press.
 *
 * V7.1 Changes:
 * - Removed blocking `waitPacketSent()` call to ensure the main loop is never
 * halted, making the button instantly responsive.
 * - Removed the spinning heartbeat indicator from the LCD display.
 *
 * V7.0 Changes:
 * - Ported all radio functions from RadioLib to RadioHead (RH_RF69).
 * - Implemented a struct-based packet for efficient and reliable transmission.
 */

// ============================ LIBRARIES ============================
#include <SPI.h>
#include <RH_RF69.h> // Using RadioHead library
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ====================== HARDWARE & SYSTEM CONFIG ===================
#define CYCLE_BUTTON_PIN    3
#define OFF_BUTTON_PIN      7
#define HEARTBEAT_INTERVAL  500       // Send current sequence every 500ms
#define LCD_TIMEOUT_MS      300000UL  // 5 minutes for LCD backlight timeout
#define DEBOUNCE_DELAY_MS   50        // 50ms for debounce

// --- RFM69 Radio (RadioHead Configuration) ---
#define RF69_FREQ           915.0
#define RF69_CS_PIN         10
#define RF69_INT_PIN        2
#define RF69_RST_PIN        9
const char ENCRYPTKEY[] = "GoMarchingBand!!";

// Initialize the RadioHead driver
RH_RF69 driver(RF69_CS_PIN, RF69_INT_PIN);

// --- I2C LCD ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// =========================== DATA & STATE ==========================
// Using a struct for radio packets is more efficient than strings.
struct RadioPacket {
    uint32_t packetCounter;
    uint8_t  sequenceId;
};

uint8_t currentSequence = 0; // 0 = off, 1-5 = sequences
unsigned long lastHeartbeatTime = 0;
uint32_t packetCounter = 0;

// --- Display ---
bool displayNeedsUpdate = true;
unsigned long lastActivityTime = 0;
bool lcdBacklightOn = true;

// --- Button Debouncing (Cycle Button) ---
byte cycleButtonState = HIGH;
byte lastCycleButtonState = HIGH;
unsigned long lastCycleDebounceTime = 0;

// --- Button Debouncing (Off Button) ---
byte offButtonState = HIGH;
byte lastOffButtonState = HIGH;
unsigned long lastOffDebounceTime = 0;

// --- Built-in LED blink ---
bool ledIsOn = false;
unsigned long ledOnTime = 0;


// ========================= TRANSMISSION ============================
void transmitSequence() {
    RadioPacket packet;
    packet.sequenceId = currentSequence;
    packet.packetCounter = ++packetCounter;

    driver.send((uint8_t*)&packet, sizeof(packet));
    
    digitalWrite(LED_BUILTIN, HIGH);
    ledIsOn = true;
    ledOnTime = millis();
}

// ======================== DISPLAY & UI =============================
void updateDisplay() {
    // Line 0: Sequence Status
    lcd.setCursor(0, 0);
    if (currentSequence == 0) {
        lcd.print(F("Sequence: OFF   ")); // Pad with spaces to clear line
    } else {
        lcd.print(F("Sequence: "));
        lcd.print(currentSequence);
        lcd.print(F("      "));
    }

    // Line 1: Packet Counter
    lcd.setCursor(0, 1);
    lcd.print(F("Pkts Sent: "));
    lcd.print(packetCounter);
    lcd.print(F("   ")); // Erase rest of line

    displayNeedsUpdate = false;
}

// ========================== BUTTON HANDLING ========================
bool checkCycleButtonPress() {
    bool pressed = false;
    int reading = digitalRead(CYCLE_BUTTON_PIN);
    if (reading != lastCycleButtonState) { lastCycleDebounceTime = millis(); }
    if ((millis() - lastCycleDebounceTime) > DEBOUNCE_DELAY_MS) {
        if (reading != cycleButtonState) {
            cycleButtonState = reading;
            if (cycleButtonState == LOW) { pressed = true; }
        }
    }
    lastCycleButtonState = reading;
    return pressed;
}

bool checkOffButtonPress() {
    bool pressed = false;
    int reading = digitalRead(OFF_BUTTON_PIN);
    if (reading != lastOffButtonState) { lastOffDebounceTime = millis(); }
    if ((millis() - lastOffDebounceTime) > DEBOUNCE_DELAY_MS) {
        if (reading != offButtonState) {
            offButtonState = reading;
            if (offButtonState == LOW) { pressed = true; }
        }
    }
    lastOffButtonState = reading;
    return pressed;
}


// =============================== SETUP =============================
void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(CYCLE_BUTTON_PIN, INPUT_PULLUP);
    pinMode(OFF_BUTTON_PIN, INPUT_PULLUP);
    pinMode(RF69_RST_PIN, OUTPUT);
    digitalWrite(RF69_RST_PIN, LOW);

    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print(F("Summerville Band"));
    lcd.setCursor(0, 1);
    lcd.print(F("Initializing..."));
    delay(1000);

    // --- RadioHead Initialization Sequence ---
    digitalWrite(RF69_RST_PIN, HIGH);
    delay(10);
    digitalWrite(RF69_RST_PIN, LOW);
    delay(10);

    if (!driver.init()) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("RADIO FAIL!"));
        while (true);
    }

    if (!driver.setFrequency(RF69_FREQ)) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("FREQ SET FAIL!"));
        while (true);
    }
    
    driver.setTxPower(20, true);
    driver.setEncryptionKey((uint8_t*)ENCRYPTKEY);

    lastActivityTime = millis();
    updateDisplay();
}

// ================================ LOOP =============================
void loop() {
    // 1. Handle Input
    if (checkCycleButtonPress()) {
        lastActivityTime = millis();
        if (!lcdBacklightOn) { lcd.backlight(); lcdBacklightOn = true; }

        currentSequence++;
        if (currentSequence > 5) {
            currentSequence = 0;
        }

        transmitSequence();
        lastHeartbeatTime = millis();
        displayNeedsUpdate = true;
    }

    if (checkOffButtonPress()) {
        lastActivityTime = millis();
        if (!lcdBacklightOn) { lcd.backlight(); lcdBacklightOn = true; }

        if (currentSequence != 0) {
            currentSequence = 0;
            transmitSequence();
            lastHeartbeatTime = millis();
            displayNeedsUpdate = true;
        }
    }

    // 2. Handle Timed Actions
    if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
        lastHeartbeatTime = millis();
        transmitSequence();
        displayNeedsUpdate = true;
    }

    if (lcdBacklightOn && (millis() - lastActivityTime > LCD_TIMEOUT_MS)) {
        lcd.noBacklight();
        lcdBacklightOn = false;
    }
    
    if (ledIsOn && (millis() - ledOnTime >= 50)) {
        digitalWrite(LED_BUILTIN, LOW);
        ledIsOn = false;
    }

    // 3. Update Display (if needed)
    if (displayNeedsUpdate) {
        updateDisplay();
    }
}
