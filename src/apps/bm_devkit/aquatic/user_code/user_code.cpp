#include "user_code.h"
#include "bm_network.h"
#include "bm_printf.h"
#include "bm_pubsub.h"
#include "bristlefin.h"
#include "bsp.h"
#include "debug.h"
#include "lwip/inet.h"
#include "nau7802.h"
#include "payload_uart.h"
#include "sensors.h"
#include "stm32_rtc.h"
#include "task_priorities.h"
#include "uptime.h"
#include "usart.h"
#include "util.h"

#define LED_ON_TIME_MS 20
#define LED_PERIOD_MS 1000
#define LINE_BUFFER_SIZE 2048 // matches what's in payload_uart.cpp

// Code is mostly sourced from rbr_coda_example/user_code.cpp

void setup(void) {
  // Setup the UART – the on-board serial driver that talks to the RS232 transceiver.
  PLUART::init(USER_TASK_PRIORITY);
  // Baud set per expected baud rate of the sensor.
  PLUART::setBaud(9600);
  // Set a line termination character per protocol of the sensor.
  PLUART::setTerminationCharacter('\n');
  // Turn on the UART.
  PLUART::enable();
  // Enable the input to the Vout power supply.
  bristlefin.enableVbus();
  // ensure Vbus stable before enable Vout with a 5ms delay.
  vTaskDelay(pdMS_TO_TICKS(5));
  // enable Vout, 12V by default.
  bristlefin.enableVout();
  bristlefin.enable5V();
}

void loop(void) {
  static u_int32_t led2OnTime = 0;
  static bool led2State = false;
  if (led2State && ((u_int32_t)uptimeGetMs() - led2OnTime >= LED_ON_TIME_MS * 20)) {
    bristlefin.setLed(2, Bristlefin::LED_OFF);
    led2State = false;
  }

  if (PLUART::lineAvailable()) {
    // read sensor data
    char lineBuffer[LINE_BUFFER_SIZE];
    size_t len = PLUART::readLine(lineBuffer, LINE_BUFFER_SIZE);
    if (len > 0) {
      printf("length: %d\n", len);
      printf("Received line: %.*s\n", (int)len, lineBuffer);
      bristlefin.setLed(2, Bristlefin::LED_GREEN);
      led2State = true;
      led2OnTime = uptimeGetMs();
    }
    // get rtc time
    RTCTimeAndDate_t time_and_date = {};
    rtcGet(&time_and_date);
    char rtcTimeBuffer[32] = {};
    rtcPrint(rtcTimeBuffer, NULL);
    printf("RTC time: %s\n", rtcTimeBuffer);
    // format and send over network
    char formattedData[2048];
    snprintf(formattedData, sizeof(formattedData), "data: %.*s time: %s", (int)len, lineBuffer,
             rtcTimeBuffer);

    if (spotter_tx_data((uint8_t *)formattedData, strlen(formattedData),
                        BM_NETWORK_TYPE_CELLULAR_ONLY)) { // Cell only for now
      printf("%llut - %s | Successfully sent Spotter transmit data request\n", uptimeGetMs(),
             rtcTimeBuffer);
    } else {
      printf("%llut - %s | Failed to send Spotter transmit data request\n", uptimeGetMs(),
             rtcTimeBuffer);
    }
  }

  // Flash led red to indicate OK status
  static u_int32_t led1PulseTimer = uptimeGetMs();
  static u_int32_t led1OnTimer = 0;
  static bool led1State = false;
  if (!led1State && ((u_int32_t)uptimeGetMs() - led1PulseTimer >= LED_PERIOD_MS)) {
    bristlefin.setLed(1, Bristlefin::LED_RED);
    led1OnTimer = uptimeGetMs();
    led1PulseTimer += LED_PERIOD_MS;
    led1State = true;
  } else if (led1State && ((u_int32_t)uptimeGetMs() - led1OnTimer >= LED_ON_TIME_MS)) {
    bristlefin.setLed(1, Bristlefin::LED_OFF);
    led1State = false;
  }
}
