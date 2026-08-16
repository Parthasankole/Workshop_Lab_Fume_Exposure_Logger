/*
  ============================================================
       WORKSHOP / LAB FUME EXPOSURE LOGGER (FIXED)
       ESP32 + MQ135 + SSD1306 OLED + Buzzer

       Concept:
       Instead of simply detecting "dangerous gas now",
       this project accumulates exposure over time.

       Exposure is represented as a RELATIVE EXPOSURE INDEX.
       It is NOT a calibrated ppm measurement.

       TWA-like calculation:

          TWA = SUM(Index * Time) / Total Time

       Cumulative exposure:

          Exposure = SUM(Index * Time)

  ------------------------------------------------------------
       CHANGES IN THIS VERSION
       1. Buzzer is now non-blocking (millis()-based state
          machine instead of delay()).
       2. Calibration waits for an MQ135 warm-up period before
          averaging the baseline.
       3. Exposure index formula is unchanged in behavior but
          refactored so the "sharper response at high gas
          levels" claim in the comments is now a real,
          adjustable curve (EXPOSURE_CURVE_POWER).
       4. Reset button debounce is now millis()-based, not
          delay()-based.
       5. OLED init failure no longer locks the device in an
          infinite buzzer loop — it falls back to serial-only
          logging.
       6. Cumulative exposure threshold units and tuning
          approach are documented inline.
  ============================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================
//                      PIN DEFINITIONS
// ============================================================

#define MQ_PIN          34      // MQ135 analog output
#define BUZZER_PIN      25      // Active buzzer
#define RESET_BUTTON    27      // Reset/New session button

// OLED I2C pins
#define OLED_SDA        21
#define OLED_SCL        22

#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64

#define OLED_RESET      -1
#define OLED_ADDRESS    0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

// Set to false at runtime if the OLED fails to initialize.
// The rest of the code checks this flag before touching the
// display, so a dead/missing OLED no longer bricks the logger.
bool oledAvailable = true;


// ============================================================
//                  CALIBRATION SETTINGS
// ============================================================

// Number of samples used during startup calibration
#define CALIBRATION_SAMPLES  100

// Delay between calibration samples
#define CALIBRATION_DELAY    100

// MQ135 needs time on power-up before readings are stable.
// A quick power-on gives you a "baseline" that is really just
// mid-warm-up drift, not clean air. 60 s is a practical
// workshop minimum — the datasheet's full burn-in is much
// longer (hours), but 60 s meaningfully reduces drift.
#define WARMUP_SECONDS        60


// ============================================================
//                  EXPOSURE SETTINGS
// ============================================================

// These are RELATIVE exposure thresholds.
// They are NOT ppm values.

// Percentage above baseline at which exposure starts
float EXPOSURE_START = 1.0;

// Percentage above baseline considered high
float HIGH_EXPOSURE = 40.0;

// Percentage above baseline considered very high
float CRITICAL_EXPOSURE = 70.0;

// The exposure index is normalized so that:
//   percentage == EXPOSURE_RANGE_MAX  ->  index == EXPOSURE_INDEX_MAX
// EXPOSURE_CURVE_POWER controls the shape between those two points:
//   1.0 = linear            (reproduces the original formula exactly)
//   1.5 - 2.0 = concave-up  (index rises slowly at first, then
//                            sharply as gas level increases —
//                            matches what the old comments claimed
//                            but the old linear formula didn't do)
#define EXPOSURE_RANGE_MAX     100.0
#define EXPOSURE_INDEX_MAX     90.0
#define EXPOSURE_CURVE_POWER   1.0


// ============================================================
//              CUMULATIVE EXPOSURE THRESHOLDS
// ============================================================

// These values are project-defined units:
//     Relative Exposure Index x seconds
//
// They are NOT a standard/calibrated unit (unlike a real TWA
// in ppm-hours), so there's no textbook number to plug in.
// Tune them experimentally for your workshop, e.g.:
//   1. Run a normal work session and log cumulativeExposure
//      over Serial.
//   2. Note the value reached during a period you'd consider
//      "should get a warning" -> set WARNING_LIMIT there.
//   3. Note the value during a period you'd consider
//      "should escalate to alarm" -> set CRITICAL_LIMIT there.
//   4. Re-tune whenever EXPOSURE_START/EXPOSURE_CURVE_POWER
//      change, since those change what the index itself means.
float WARNING_LIMIT = 2000.0;
float CRITICAL_LIMIT = 3000.0;


// ============================================================
//                      VARIABLES
// ============================================================

float baseline = 0;

float gasReading = 0;

float exposureIndex = 0;

float cumulativeExposure = 0;

float twa = 0;

unsigned long sessionStartTime = 0;
unsigned long lastSampleTime = 0;

unsigned long totalExposureTime = 0;

unsigned long lastDisplayUpdate = 0;

unsigned long lastBeepTime = 0;


// ============================================================
//                    EXPOSURE STATES
// ============================================================

enum ExposureState
{
  STATE_NORMAL,
  STATE_ELEVATED,
  STATE_HIGH,
  STATE_CRITICAL
};

ExposureState currentState = STATE_NORMAL;


// ============================================================
//                  BUZZER STATE MACHINE
// ============================================================
// Non-blocking replacement for the old delay()-based buzzer.
// Each state's beep pattern is expressed as a small sequence
// of phases, advanced only when enough time has passed —
// never blocking loop().

enum BuzzerPhase
{
  PHASE_WAIT,
  PHASE_BEEP_ON,
  PHASE_GAP,
  PHASE_BEEP2_ON
};

BuzzerPhase buzzerPhase = PHASE_WAIT;
unsigned long buzzerPhaseStart = 0;


// ============================================================
//                    READ MQ SENSOR
// ============================================================

float readMQSensor()
{
  long total = 0;

  // Average several ADC readings
  for (int i = 0; i < 10; i++)
  {
    total += analogRead(MQ_PIN);
    delay(2);
  }

  return total / 10.0;
}


// ============================================================
//                  CALIBRATE BASELINE
// ============================================================

void calibrateSensor()
{
  float total = 0;

  // ----------------------------------------------------------
  // WARM-UP PERIOD
  // ----------------------------------------------------------
  // MQ-series sensors drift for a while after power-up. Wait
  // here (with a visible countdown) before we start averaging
  // the baseline, or "clean air" will actually be "warming-up
  // air" and every later percentage-above-baseline reading
  // will be skewed.

  for (int remaining = WARMUP_SECONDS; remaining > 0; remaining--)
  {
    if (oledAvailable)
    {
      display.clearDisplay();

      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);

      display.setCursor(0, 0);
      display.println("FUME LOGGER");

      display.setCursor(0, 15);
      display.println("Sensor warming up...");

      display.setCursor(0, 30);
      display.print("Wait: ");
      display.print(remaining);
      display.println("s");

      display.display();
    }

    Serial.print("Warm-up: ");
    Serial.print(remaining);
    Serial.println("s remaining");

    delay(1000);
  }

  // ----------------------------------------------------------
  // BASELINE AVERAGING
  // ----------------------------------------------------------

  if (oledAvailable)
  {
    display.clearDisplay();

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.println("FUME LOGGER");

    display.setCursor(0, 15);
    display.println("Calibrating...");

    display.setCursor(0, 30);
    display.println("Keep sensor in");

    display.setCursor(0, 42);
    display.println("clean air.");

    display.display();
  }

  Serial.println();
  Serial.println("==============================");
  Serial.println("MQ SENSOR CALIBRATION");
  Serial.println("==============================");
  Serial.println("Keep sensor in relatively");
  Serial.println("clean air...");
  Serial.println();

  for (int i = 0; i < CALIBRATION_SAMPLES; i++)
  {
    float reading = analogRead(MQ_PIN);

    total += reading;

    delay(CALIBRATION_DELAY);
  }

  baseline = total / CALIBRATION_SAMPLES;

  Serial.print("Baseline = ");
  Serial.println(baseline);

  if (oledAvailable)
  {
    display.clearDisplay();

    display.setCursor(0, 0);
    display.println("CALIBRATION DONE");

    display.setCursor(0, 20);
    display.print("Baseline: ");
    display.println(baseline, 0);

    display.display();
  }

  delay(2000);
}


// ============================================================
//              CALCULATE EXPOSURE INDEX
// ============================================================

float calculateExposure(float reading)
{
  if (reading <= baseline)
  {
    return 0;
  }

  // Calculate percentage above baseline
  float percentage =
      ((reading - baseline) / baseline) * 100.0;

  // Ignore small changes
  if (percentage < EXPOSURE_START)
  {
    return 0;
  }

  /*
      Map percentage-above-baseline onto the exposure index.

      normalized = 0 at percentage == EXPOSURE_START
      normalized = 1 at percentage == EXPOSURE_RANGE_MAX

      index = normalized^EXPOSURE_CURVE_POWER * EXPOSURE_INDEX_MAX

      With EXPOSURE_CURVE_POWER = 1.0 this reproduces the
      original numbers exactly:
          20% above baseline  -> index = 10
          50% above baseline  -> index = 40
          100% above baseline -> index = 90
      Raising EXPOSURE_CURVE_POWER above 1.0 makes the index
      grow slowly at first and then sharply near/above
      EXPOSURE_RANGE_MAX, for a genuinely nonlinear response.
      Readings above EXPOSURE_RANGE_MAX keep extrapolating
      along the same curve rather than clipping, so a serious
      spike still shows as a much larger index.
  */

  float range = percentage - EXPOSURE_START;
  float maxRange = EXPOSURE_RANGE_MAX - EXPOSURE_START;

  float normalized = range / maxRange;

  float index = pow(normalized, EXPOSURE_CURVE_POWER) * EXPOSURE_INDEX_MAX;

  if (index < 0)
  {
    index = 0;
  }

  return index;
}


// ============================================================
//                  DETERMINE STATE
// ============================================================

void determineState()
{
  if (cumulativeExposure >= CRITICAL_LIMIT ||
      exposureIndex >= CRITICAL_EXPOSURE)
  {
    currentState = STATE_CRITICAL;
  }

  else if (cumulativeExposure >= WARNING_LIMIT ||
           exposureIndex >= HIGH_EXPOSURE)
  {
    currentState = STATE_HIGH;
  }

  else if (exposureIndex > 0)
  {
    currentState = STATE_ELEVATED;
  }

  else
  {
    currentState = STATE_NORMAL;
  }
}


// ============================================================
//                    UPDATE EXPOSURE
// ============================================================

void updateExposure()
{
  unsigned long currentTime = millis();

  unsigned long elapsed =
      currentTime - lastSampleTime;

  // Convert milliseconds to seconds
  float elapsedSeconds =
      elapsed / 1000.0;

  // Don't update if less than 1 second
  if (elapsedSeconds < 1.0)
  {
    return;
  }

  lastSampleTime = currentTime;

  // Read sensor
  gasReading = readMQSensor();

  // Calculate relative exposure
  exposureIndex =
      calculateExposure(gasReading);


  // ========================================================
  //            CUMULATIVE EXPOSURE CALCULATION
  // ========================================================

  cumulativeExposure +=
      exposureIndex * elapsedSeconds;


  // ========================================================
  //              EXPOSURE TIME
  // ========================================================

  if (exposureIndex > 0)
  {
    totalExposureTime +=
        (unsigned long)elapsedSeconds;
  }


  // ========================================================
  //                  TWA CALCULATION
  // ========================================================

  unsigned long sessionTime =
      millis() - sessionStartTime;

  float sessionSeconds =
      sessionTime / 1000.0;

  if (sessionSeconds > 0)
  {
    twa =
        cumulativeExposure / sessionSeconds;
  }


  // Determine current state
  determineState();
}


// ============================================================
//                       BUZZER
// ============================================================
// Fully non-blocking. Called every loop() iteration; it only
// toggles the pin and advances buzzerPhase when the relevant
// time window has elapsed, so sensor reads / display updates /
// button checks never stall while a beep pattern is playing.

void updateBuzzer()
{
  if (currentState == STATE_CRITICAL)
  {
    digitalWrite(BUZZER_PIN, HIGH);   // keep beeping, no stop
  }
  else if (currentState == STATE_HIGH)
  {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(2000);                      // beep 2 sec
    digitalWrite(BUZZER_PIN, LOW);    // then stop
  }
  else
  {
    digitalWrite(BUZZER_PIN, LOW);    // NORMAL/ELEVATED = silent
  }
}


// ============================================================
//                    OLED DISPLAY
// ============================================================

void updateDisplay()
{
  if (!oledAvailable)
  {
    return;
  }

  unsigned long currentTime = millis();

  // Update OLED every 500 ms
  if (currentTime - lastDisplayUpdate < 500)
  {
    return;
  }

  lastDisplayUpdate = currentTime;


  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);


  // ========================================================
  // TITLE
  // ========================================================

  display.setTextSize(1);

  display.setCursor(0, 0);

  display.println("FUME EXPOSURE LOGGER");


  // ========================================================
  // GAS READING
  // ========================================================

  display.setCursor(0, 12);

  display.print("Gas: ");

  display.println(gasReading, 0);


  // ========================================================
  // EXPOSURE INDEX
  // ========================================================

  display.setCursor(0, 22);

  display.print("Index: ");

  display.println(exposureIndex, 1);


  // ========================================================
  // CUMULATIVE EXPOSURE
  // ========================================================

  display.setCursor(0, 32);

  display.print("Exposure: ");

  display.println(cumulativeExposure, 0);


  // ========================================================
  // TWA
  // ========================================================

  display.setCursor(0, 42);

  display.print("TWA: ");

  display.println(twa, 1);


  // ========================================================
  // STATUS
  // ========================================================

  display.setCursor(0, 54);

  display.print("Status: ");

  switch (currentState)
  {
    case STATE_NORMAL:
      display.print("NORMAL");
      break;

    case STATE_ELEVATED:
      display.print("ELEVATED");
      break;

    case STATE_HIGH:
      display.print("HIGH");
      break;

    case STATE_CRITICAL:
      display.print("CRITICAL");
      break;
  }

  display.display();
}


// ============================================================
//                    RESET SESSION
// ============================================================

void resetSession()
{
  cumulativeExposure = 0;

  twa = 0;

  totalExposureTime = 0;

  sessionStartTime = millis();

  lastSampleTime = millis();

  currentState = STATE_NORMAL;

  lastBeepTime = millis();

  buzzerPhase = PHASE_WAIT;

  digitalWrite(BUZZER_PIN, LOW);

  Serial.println();
  Serial.println("==============================");
  Serial.println("NEW EXPOSURE SESSION");
  Serial.println("==============================");
}


// ============================================================
//                    CHECK BUTTON
// ============================================================
// Non-blocking debounce: instead of delay(30), we track how
// long the raw pin reading has been stable and only act once
// it has held steady for DEBOUNCE_DELAY ms. A short bounce
// keeps resetting the timer, so it can't misfire mid-bounce,
// and loop() never stalls waiting on it.

void checkResetButton()
{
  const unsigned long DEBOUNCE_DELAY = 30;

  static bool lastRawState = HIGH;
  static bool stableState = HIGH;
  static unsigned long lastChangeTime = 0;

  bool rawState = digitalRead(RESET_BUTTON);

  if (rawState != lastRawState)
  {
    lastChangeTime = millis();
    lastRawState = rawState;
  }

  if ((millis() - lastChangeTime) >= DEBOUNCE_DELAY)
  {
    if (stableState != rawState)
    {
      stableState = rawState;

      // Detect press (HIGH -> LOW, pin uses INPUT_PULLUP)
      if (stableState == LOW)
      {
        resetSession();
      }
    }
  }
}


// ============================================================
//                  SERIAL MONITOR
// ============================================================

void printSerialData()
{
  static unsigned long lastPrint = 0;

  if (millis() - lastPrint < 2000)
  {
    return;
  }

  lastPrint = millis();

  Serial.print("Gas = ");
  Serial.print(gasReading);

  Serial.print(" | Index = ");
  Serial.print(exposureIndex);

  Serial.print(" | Cumulative = ");
  Serial.print(cumulativeExposure);

  Serial.print(" | TWA = ");
  Serial.print(twa);

  Serial.print(" | Status = ");

  switch (currentState)
  {
    case STATE_NORMAL:
      Serial.println("NORMAL");
      break;

    case STATE_ELEVATED:
      Serial.println("ELEVATED");
      break;

    case STATE_HIGH:
      Serial.println("HIGH");
      break;

    case STATE_CRITICAL:
      Serial.println("CRITICAL");
      break;
  }

  if (!oledAvailable)
  {
    Serial.println("(Running in serial-only fallback mode — OLED not detected)");
  }
}


// ============================================================
//                         SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(1000);

  // Configure pins
  pinMode(MQ_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(
    RESET_BUTTON,
    INPUT_PULLUP
  );

  digitalWrite(
    BUZZER_PIN,
    LOW
  );


  // ========================================================
  // OLED INITIALIZATION
  // ========================================================
  // If the OLED fails to start, we no longer lock the device
  // in an infinite alarm loop. We log it once over Serial,
  // set oledAvailable = false, and continue running normally
  // in serial-only mode — every display.*() call elsewhere is
  // guarded by that flag.

  Wire.begin(
    OLED_SDA,
    OLED_SCL
  );

  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDRESS))
  {
    oledAvailable = false;

    Serial.println(
      "OLED initialization failed! Continuing in serial-only mode."
    );
  }


  // Startup screen
  if (oledAvailable)
  {
    display.clearDisplay();

    display.setTextColor(
      SSD1306_WHITE
    );

    display.setTextSize(1);

    display.setCursor(0, 10);

    display.println(
      "FUME EXPOSURE LOGGER"
    );

    display.setCursor(0, 30);

    display.println(
      "Starting..."
    );

    display.display();

    delay(2000);
  }


  // ========================================================
  // CALIBRATION
  // ========================================================

  calibrateSensor();


  // ========================================================
  // START SESSION
  // ========================================================

  sessionStartTime =
      millis();

  lastSampleTime =
      millis();

  lastBeepTime =
      millis();


  Serial.println();
  Serial.println("==============================");
  Serial.println("FUME EXPOSURE LOGGER READY");
  Serial.println("==============================");
  Serial.println();

  Serial.print(
    "Baseline = "
  );

  Serial.println(
    baseline
  );
}


// ============================================================
//                          LOOP
// ============================================================

void loop()
{
  // Check reset button
  checkResetButton();


  // Read sensor and calculate exposure
  updateExposure();


  // Update buzzer
  updateBuzzer();


  // Update OLED
  updateDisplay();


  // Send information to Serial Monitor
  printSerialData();
}
