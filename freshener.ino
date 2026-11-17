#include <LiquidCrystal.h> // lcd library
#include <NewPing.h> // distance sensor library
#include <OneWire.h> // temperature sensor library
#include <DallasTemperature.h> // temperature sensor library
#include <EEPROM.h> // use EEPROM to save remaining sprays

// =======================
// ENUMS and STRUCTS
// =======================

enum MenuButton {
  BTN_NONE,
  BTN_1,
  BTN_2
};

enum State {
  NOT_IN_USE,
  IN_USE_UNKNOWN,
  IN_USE_1OR2,
  IN_USE_1,
  IN_USE_2,
  IN_USE_CLEANING,
  TRIGGERED_SPRAY_IMMINENT,
  OPERATOR_MENU_ACTIVE
};

enum FlushType {
  FLUSH_NONE,
  FLUSH_BIG,
  FLUSH_SMALL
};

enum MenuMode {
  MENU_MAIN,
  MENU_EDIT_DELAY
};

struct RgbPattern {
  bool r;
  bool g;
  bool b;
  unsigned long blinkMs; // 0 = steady, >0 = blink period
};

// =======================
// PIN VARIABLES
// =======================

// Interrupts
const byte PIN_REED = 2; // magnetic
const byte PIN_BTN = 3; // override button

// LCD
const byte LCD_RS = 4;
const byte LCD_E  = 5;
const byte LCD_D4 = 6;
const byte LCD_D5 = 7;
const byte LCD_D6 = 8;
const byte LCD_D7 = 9;

// Sensors
const byte PIN_BTNS = A2; // menu buttons
const byte PIN_LDR  = A0; // light
const byte PIN_PIR  = A4; // movement
const byte PIN_TEMP = A3; // temperature
const byte PIN_TRIG = A5; // distance (trig)
const byte PIN_ECHO = A1; // distance (echo)

// Output
const byte PIN_FRESHENER = 13;

// RGB LED
const byte PIN_R = 10;
const byte PIN_G = 12;
const byte PIN_B = 11;

// =======================
// GLOBAL VARIABLES
// =======================

uint16_t remainingSprays = 2400;
bool doorOpen = false;
byte sprayCount = 0;
bool spray2 = false;
bool sprayNow = false;
unsigned long sprayNowStartMs = 0;
float temperature = 0;
unsigned long lastTempUpdateMs = 0;
const unsigned long TEMP_INTERVAL = 1000UL;
unsigned long lastLcdUpdateMs = 0;
const unsigned long LCD_INTERVAL = 500UL;

// =======================
// LIBRARIES SETUP
// =======================

LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7); // lcd
NewPing sonar(PIN_TRIG, PIN_ECHO, 200); // distance
OneWire oneWire(PIN_TEMP); // temperature
DallasTemperature sensors(&oneWire); // temperature

// =======================
// INTERRUPTS
// =======================
volatile bool reedChanged = false;
void isrReed() { reedChanged = true; }

volatile bool overridePressed = false;
volatile unsigned long lastOverrideMs = 0;
void isrOverride() {
  unsigned long now = millis();
  if (now - lastOverrideMs > 50) {
    lastOverrideMs = now;
    overridePressed = true;
  }
}

// =======================
// HELPER FUNCTIONS
// =======================

// Transfer states and keep track of when this happened
State state = NOT_IN_USE;
unsigned long stateEnteredMs = 0;
void enterState(State s) {
  state = s;
  stateEnteredMs = millis();

  if (s == TRIGGERED_SPRAY_IMMINENT) {
    sprayNow = false;
    sprayNowStartMs = stateEnteredMs;
    digitalWrite(PIN_FRESHENER, LOW);
  }
}

// Set all rgb pins with one function
void setRGB(bool r, bool g, bool b) {
  digitalWrite(PIN_R, r ? HIGH : LOW);
  digitalWrite(PIN_G, g ? HIGH : LOW);
  digitalWrite(PIN_B, b ? HIGH : LOW);
}

// function to map states to rgb led functionality
RgbPattern getPatternForState(State s) {
  switch (s) {
    case NOT_IN_USE:              return {0, 0, 0, 0};       // off
    case IN_USE_UNKNOWN:          return {0, 0, 1, 0};       // blue steady
    case IN_USE_1OR2:             return {0, 1, 1, 0};       // cyan steady
    case IN_USE_1:                return {0, 1, 0, 0};       // green steady
    case IN_USE_2:                return {1, 0, 0, 0};       // red steady
    case IN_USE_CLEANING:         return {1, 0, 1, 0};     // purple
    case TRIGGERED_SPRAY_IMMINENT:return {1, 1, 0, 250};     // yellow fast blink
    case OPERATOR_MENU_ACTIVE:    return {.25, .25, .25};    // white
    default:                      return {1, 1, 1, 100};     // fallback white
  }
}

// update the rgb based on the state
void updateRgbForState(State s, unsigned long now) {
  static State lastState = NOT_IN_USE;
  static bool blinkOn = true;
  static unsigned long lastToggleMs = 0;

  RgbPattern p = getPatternForState(s);

  // if state changed, reset blinking timer for fresh start
  if (s != lastState) {
    lastState = s;
    blinkOn = true;
    lastToggleMs = now;
  }

  if (p.blinkMs == 0) {
    setRGB(p.r, p.g, p.b);
    return;
  }

  // blink
  if (now - lastToggleMs >= (p.blinkMs / 2)) {
    lastToggleMs = now;
    blinkOn = !blinkOn;
  }

  if (blinkOn) setRGB(p.r, p.g, p.b);
  else         setRGB(false, false, false);
}

// Check if the light is above a certain threshold
bool isLightOn(int light) {
  if (light > 400){
    return true;
  } else {
    return false;
  }
}

// Print the current state to the LCD
void updateLCD(State state) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(temperature, 1);
  lcd.write(223);
  lcd.print("C      ");
  lcd.print(remainingSprays);

  lcd.setCursor(0, 1);

  switch (state) {
    case NOT_IN_USE:
      lcd.print("NOT_IN_USE");
      break;

    case IN_USE_UNKNOWN:
      lcd.print("IN_USE_UNKNOWN");
      break;

    case IN_USE_1OR2:
      lcd.print("IN_USE_1OR2");
      break;

    case IN_USE_1:
      lcd.print("IN_USE_1");
      break;

    case IN_USE_2:
      lcd.print("IN_USE_2");
      break;

    case IN_USE_CLEANING:
      lcd.print("CLEANING");
      break;

    case TRIGGERED_SPRAY_IMMINENT:
      lcd.print("SPRAY_IMMINENT");
      break;

    case OPERATOR_MENU_ACTIVE:
      lcd.print("MENU");
      break;

    default:
      lcd.print("UNKNOWN");
      break;
  }
}

// Read the raw values of the menu buttons (only used by readMenuButtonDebounced() )
MenuButton readMenuButton() {
  int value = analogRead(PIN_BTNS);

  if (value < 100) return BTN_1;
  if (value > 400 && value < 700) return BTN_2;
  return BTN_NONE;
}

// Debounce the menu buttons
MenuButton readMenuButtonDebounced() {
  static MenuButton buttonState = BTN_NONE;
  static MenuButton lastReading = BTN_NONE;
  static unsigned long lastDebounceTime = 0;

  const unsigned long debounceDelay = 50;

  MenuButton reading = readMenuButton();

  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {

    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState != BTN_NONE) {
        lastReading = reading;
        return buttonState;
      }
    }
  }

  lastReading = reading;
  return BTN_NONE;
}

// Translate distance measured to a certain type of flush
FlushType readFlushRaw(unsigned int cm) {
  if (cm == 0) return FLUSH_NONE;
  if (cm < 18) return FLUSH_SMALL;
  if (cm < 30) return FLUSH_BIG;
  return FLUSH_NONE;
}

// Debounce flush events
FlushType readFlushEvent() {
  const unsigned long DEBOUNCE_MS = 80;
  const unsigned long LOCKOUT_MS  = 800;

  static FlushType lastRaw = FLUSH_NONE;
  static FlushType stable = FLUSH_NONE;
  static unsigned long lastChangeMs = 0;
  static unsigned long lastEventMs = 0;

  unsigned long now = millis();

  unsigned int cm = sonar.ping_cm();
  FlushType raw = readFlushRaw(cm);

  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeMs = now;
  }

  if ((now - lastChangeMs) >= DEBOUNCE_MS && raw != stable) {
    stable = raw;

    if (stable != FLUSH_NONE && (now - lastEventMs) >= LOCKOUT_MS) {
      lastEventMs = now;
      return stable;
    }
  }

  return FLUSH_NONE;
}

// =======================
// MENU SYSTEM
// =======================

MenuMode menuMode = MENU_MAIN;
byte menuIndex = 0; // 0=Delay / 1=Reset / 2=Exitw
unsigned long lastMenuActionMs = 0; // keeping track of the last action for timeout
const unsigned long MENU_TIMEOUT_MS = 20000UL; // timeout after 20s of no action

const unsigned long DELAY_OPTIONS[] = { // delay options for the user
  0UL, 2000UL, 5000UL, 10000UL, 20000UL, 30000UL, 60000UL
};
const byte NUM_DELAYS = sizeof(DELAY_OPTIONS) / sizeof(DELAY_OPTIONS[0]);
byte delayIndex = 2; // default delay is 5s
bool ignoreNextMenuEvent = false;

// function to start the menu
void startMenu() {
  menuMode = MENU_MAIN; // start in main mode
  menuIndex = 0; // delay option is pre-selected
  lastMenuActionMs = millis(); // start timeout timer

  // clear the lcd
  lcd.clear();
}

// function to display the main menu: first line is the selected option, second line is button labels
void drawMenuMain() {
  lcd.setCursor(0, 0);
  lcd.print(">"); // acts as a cursor
  if (menuIndex == 0) { // first option shows the current delay
    lcd.print("Delay: ");
    lcd.print(DELAY_OPTIONS[delayIndex] / 1000);
    lcd.print(" sec    ");
    lcd.setCursor(0, 1);
    lcd.print(" Next    Select ");
  } else if (menuIndex == 1) { // second option shows the amount of sprays and the option to reset
    lcd.print("Uses left: ");
    lcd.print(remainingSprays);
    lcd.setCursor(0, 1);
    lcd.print(" Next     Reset ");
  } else { // third option exits the menu
    lcd.print("Exit           ");
    lcd.setCursor(0, 1);
    lcd.print(" Next    Select ");
  }
}

// function to display the edit delay menu
void drawMenuEditDelay() {
  lcd.setCursor(0, 0);
  lcd.print("Edit delay: ");
  lcd.print(DELAY_OPTIONS[delayIndex] / 1000);
  lcd.print("s  ");
  lcd.setCursor(0, 1);
  lcd.print(" Next      Save ");
}

// button process function for the menu, returns true when the menu should exit
bool runMenu(MenuButton evt) {
  unsigned long now = millis();

  // check for timeout, if so, exit menu
  if (now - lastMenuActionMs >= MENU_TIMEOUT_MS) {
    return true;
  }

  // if there's no new input, just render the menu
  if (evt == BTN_NONE) {
    if (menuMode == MENU_MAIN) drawMenuMain();
    else drawMenuEditDelay();
    return false;
  }

  // if there is new input, reset the last action timer
  lastMenuActionMs = now;

  // BTN_1 = browse / BTN_2 = select
  if (menuMode == MENU_MAIN) {
    if (evt == BTN_1) {
      menuIndex = (menuIndex + 1) % 3; // cycle through the items with the 1st button
    } else if (evt == BTN_2) { // second button selects the option on the cursor
      if (menuIndex == 0) {
        menuMode = MENU_EDIT_DELAY; // if delay is selected, change the menu mode
      } else if (menuIndex == 1) {
        remainingSprays = 2400; // if reset sprays is selected, reset the number
        EEPROM.put(0, remainingSprays);
      } else {
        return true; // when exit is selected, return true
      }
    }

    drawMenuMain();
    return false;
  }

  // MENU_EDIT_DELAY
  if (menuMode == MENU_EDIT_DELAY) {
    if (evt == BTN_1) {
      delayIndex = (delayIndex + 1) % NUM_DELAYS; // cycle through the delay options with the 1st button
    } else if (evt == BTN_2) { // save the selected option with the 2nd button and go back to MENU_MAIN
      menuMode = MENU_MAIN;
      EEPROM.put(2, delayIndex);
    }

    drawMenuEditDelay();
    return false;
  }

  return false;
}


// =======================
// SETUP
// =======================
void setup() {
  Serial.begin(9600);

  // Inputs
  pinMode(PIN_REED, INPUT);
  pinMode(PIN_BTN, INPUT);
  pinMode(PIN_BTNS, INPUT);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LDR, INPUT);

  // Outputs
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_FRESHENER, OUTPUT);
  digitalWrite(PIN_FRESHENER, LOW);

  // Interrupts
  attachInterrupt(digitalPinToInterrupt(PIN_REED), isrReed, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), isrOverride, FALLING);

  // LCD
  lcd.begin(16, 2);
  lcd.clear();

  // Temperature sensor
  sensors.begin();

  // State variables
  doorOpen = digitalRead(PIN_REED);
  
  EEPROM.get(0, remainingSprays);
  if (remainingSprays >= 2400) { // if impossible value, reset to default
    remainingSprays = 2400;
    EEPROM.put(0, remainingSprays);
  }
  EEPROM.get(2, delayIndex);
  if (delayIndex >= NUM_DELAYS) {
    delayIndex = 2; // if impossible value reset to default
    EEPROM.put(2, delayIndex);
  }
}

// =======================
// LOOP
// =======================
void loop() {
  unsigned long now = millis();
  updateRgbForState(state, now);

  // check for interrupts and save them for this loop
  bool reedEvt = false;
  bool overrideEvt = false;

  noInterrupts();
  reedEvt = reedChanged; reedChanged = false;
  overrideEvt = overridePressed; overridePressed = false;
  interrupts();

  if (reedEvt) {doorOpen = digitalRead(PIN_REED);}

  // manual override button triggers the spray state with one spray
  if (overrideEvt) {
    sprayCount = 1;
    enterState(TRIGGERED_SPRAY_IMMINENT);
  }

  FlushType flushEv = readFlushEvent();

  bool flushSmall = (flushEv == FLUSH_SMALL);
  bool flushBig = (flushEv == FLUSH_BIG);

  // read inputs
  MenuButton menuEvt = readMenuButtonDebounced(); // BTN_NONE/BTN_1/BTN_2
  bool motion = digitalRead(PIN_PIR); // 0 = no motion / 1 = motion
  int light = analogRead(PIN_LDR);
  if (now - lastTempUpdateMs >= TEMP_INTERVAL) {
    lastTempUpdateMs = now;
    sensors.requestTemperatures();
    temperature = sensors.getTempCByIndex(0);
  }

  // if a menu button is pressed, enter OPERATOR_MENU_ACTIVE state and start the menu
  if (state != OPERATOR_MENU_ACTIVE && menuEvt != BTN_NONE) {
    enterState(OPERATOR_MENU_ACTIVE);
    startMenu();

    // ignore the first button press, as this was used to start up the menu
    ignoreNextMenuEvent = true;
  }

  // state machine
  switch (state) {
    case OPERATOR_MENU_ACTIVE: {
      // ignore the first button press, as this was used to start up the menu
      if (ignoreNextMenuEvent) {
      ignoreNextMenuEvent = false;
      menuEvt = BTN_NONE;
      }

      // pass other button presses to the menu function to process
      bool shouldExit = runMenu(menuEvt);

      // the menu function returns true whenever it should exit
      if (shouldExit) enterState(NOT_IN_USE);
      return;}

    case NOT_IN_USE:
      // use starts when the door opens, the light is on and motion is detected, go to state IN_USE_UNKNOWN
      if (doorOpen && isLightOn(light) && motion) {
        enterState(IN_USE_UNKNOWN);
      }
      break;

    case IN_USE_UNKNOWN:
      // flushing before closing the door means the toilet has been cleaned, go to state IN_USE_CLEANING
      if (flushSmall || flushBig) {
        enterState(IN_USE_CLEANING);
      }
      // door closing while the light is still on means someone is using the bathroom for either #1 or #2, go to state IN_USE_1OR2
      else if (!doorOpen && isLightOn(light)) {
        enterState(IN_USE_1OR2);
      }
      // door closing while the light is off means the user has left the bathroom, go to state NOT_IN_USE
      else if (!doorOpen && !isLightOn(light) && !motion)
        enterState(NOT_IN_USE);
      break;

    case IN_USE_CLEANING:
      // cleaning is done once the door closes, the light is turned off and there's no motion, go to state NOT_IN_USE
      if (!doorOpen && !isLightOn(light) && !motion) {
        enterState(NOT_IN_USE);
      }
      break;

    case IN_USE_1OR2:
      // if the small flush button was used, this indicates #1 use, go to state IN_USE_1
      if (flushSmall) enterState(IN_USE_1);
      // if the big flush button was used, this indicates #2 use, go to state IN_USE_2
      else if (flushBig) enterState(IN_USE_2);
      // if there's no motion and the light turns off before flushing we have probably 
      // falsely assumed it was in use for #1 or #2, go to state NOT_IN_USE
      else if (!doorOpen && !isLightOn(light) && !motion) enterState(NOT_IN_USE);
      break;

    case IN_USE_1:
      // when the door is closed, the light is off and there's no motion, 
      // we can assume the user has left the bathroom, go to state TRIGGERED_SPRAY_IMMINENT
      if (!doorOpen && !isLightOn(light) && !motion) {
        sprayCount = 1;
        sprayNow = false;
        sprayNowStartMs = now;
        enterState(TRIGGERED_SPRAY_IMMINENT);
      }
      break;

    case IN_USE_2:
    // when the door is closed, the light is off and there's no motion, 
    // we can assume the user has left the bathroom, go to state TRIGGERED_SPRAY_IMMINENT
      if (!doorOpen && !isLightOn(light) && !motion) {
        sprayCount = 2;
        enterState(TRIGGERED_SPRAY_IMMINENT);
      }
      break;

    case TRIGGERED_SPRAY_IMMINENT: {
      // if the userdelay hasn't passed yet, check if it has now and if so, set the sprayNow to true 
      // set freshener pin to high and start the spray timer
      if (!sprayNow) { // for a second spray we add a 2 sec timer instead of the user delay
        if ((!spray2 && now - sprayNowStartMs >= DELAY_OPTIONS[delayIndex]) || (spray2 && now - sprayNowStartMs >= 2000UL )) { 
          digitalWrite(PIN_FRESHENER, HIGH);
          sprayNow = true;
          sprayNowStartMs = now;
          spray2 = false;
        }
      }
      // if the userdelay has passed and the freshener has been on for 20 seconds we can turn it off
      // if a second spray was requested (#2 use) we stay in this state and reset the timer
      else {
        if (now - sprayNowStartMs >= 20000UL) {
          digitalWrite(PIN_FRESHENER, LOW);
          if (remainingSprays > 0) {
            remainingSprays--;
            EEPROM.put(0, remainingSprays);
          }

          sprayNow = false;

          if (sprayCount > 0) sprayCount--;

          if (sprayCount > 0) {
            spray2 = true;
            sprayNowStartMs = now();
          } else {
            sprayNow = false;
            spray2 = false;
            enterState(NOT_IN_USE);
          }
        }
      }

      break;
    }

    // if, for some reason, we are not in any of the known states, go to state NOT_IN_USE
    default:
      enterState(NOT_IN_USE);
      break;
  }

  // update the LCD
  if (now - lastLcdUpdateMs >= LCD_INTERVAL) {
    lastLcdUpdateMs = now;
    updateLCD(state);
  }

}
