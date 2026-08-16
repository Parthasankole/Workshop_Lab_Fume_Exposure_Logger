
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define MQ_PIN          34      // MQ135 analog output
#define BUZZER_PIN      25      // Active buzzer
#define RESET_BUTTON    27      // Reset/New session button
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

bool oledAvailable = true;
#define CALIBRATION_SAMPLES  100
#define CALIBRATION_DELAY    100
#define WARMUP_SECONDS        60
float EXPOSURE_START = 1.0;
float HIGH_EXPOSURE = 40.0;
float CRITICAL_EXPOSURE = 70.0;
#define EXPOSURE_RANGE_MAX     100.0
#define EXPOSURE_INDEX_MAX     90.0
#define EXPOSURE_CURVE_POWER   1.0
float WARNING_LIMIT = 2000.0;
float CRITICAL_LIMIT = 3000.0;
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
enum ExposureState
{
  STATE_NORMAL,
  STATE_ELEVATED,
  STATE_HIGH,
  STATE_CRITICAL
};
ExposureState currentState = STATE_NORMAL;
enum BuzzerPhase
{
  PHASE_WAIT,
  PHASE_BEEP_ON,
  PHASE_GAP,
  PHASE_BEEP2_ON
};

BuzzerPhase buzzerPhase = PHASE_WAIT;
unsigned long buzzerPhaseStart = 0;
float readMQSensor()
{
  long total = 0;
  for (int i = 0; i < 10; i++)
  {
    total += analogRead(MQ_PIN);
    delay(2);
  }

  return total / 10.0;
}
void calibrateSensor()
{
  float total = 0;
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
float calculateExposure(float reading)
{
  if (reading <= baseline)
  {
    return 0;
  }
  float percentage =
      ((reading - baseline) / baseline) * 100.0;
  if (percentage < EXPOSURE_START)
  {
    return 0;
  }
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
void updateExposure()
{
  unsigned long currentTime = millis();

  unsigned long elapsed =
      currentTime - lastSampleTime;
  float elapsedSeconds =
      elapsed / 1000.0;
  if (elapsedSeconds < 1.0)
  {
    return;
  }

  lastSampleTime = currentTime;
  gasReading = readMQSensor();
  exposureIndex =
      calculateExposure(gasReading);
  cumulativeExposure +=
      exposureIndex * elapsedSeconds;
  if (exposureIndex > 0)
  {
    totalExposureTime +=
        (unsigned long)elapsedSeconds;
  }
  unsigned long sessionTime =
      millis() - sessionStartTime;

  float sessionSeconds =
      sessionTime / 1000.0;

  if (sessionSeconds > 0)
  {
    twa =
        cumulativeExposure / sessionSeconds;
  }
  determineState();
}
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
void updateDisplay()
{
  if (!oledAvailable)
  {
    return;
  }

  unsigned long currentTime = millis();
  if (currentTime - lastDisplayUpdate < 500)
  {
    return;
  }

  lastDisplayUpdate = currentTime;


  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);

  display.println("FUME EXPOSURE LOGGER");

  display.setCursor(0, 12);

  display.print("Gas: ");

  display.println(gasReading, 0);

  display.setCursor(0, 22);

  display.print("Index: ");

  display.println(exposureIndex, 1);

  display.setCursor(0, 32);

  display.print("Exposure: ");

  display.println(cumulativeExposure, 0);

  display.setCursor(0, 42);

  display.print("TWA: ");

  display.println(twa, 1);
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

  calibrateSensor();

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
