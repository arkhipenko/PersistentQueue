/*
  Store-and-forward with at-least-once delivery.

  A reading is stored every second. Every other 10 seconds the "link" is up and the
  stored readings are sent, oldest first.

  peek() reads the oldest message and leaves it on flash. drop() removes it after the
  send succeeded. A reset between peek() and drop() sends that message again after the
  restart, so a message is never lost (it may be sent twice).

  setMaxMessages(50, true) keeps the newest 50 readings when the link stays down.

  Uses SPIFFS (the default). For LittleFS see LittleFS_Example.
*/
#include <Arduino.h>
#include <PersistentQueue.h>

PersistentQueue queue;              // oldest first, CRC on

// stand-in for a network connection: up every other 10 seconds
bool linkUp() {
  return ( millis() / 10000 ) % 2 == 1;
}

// stand-in for an MQTT publish or an HTTP post
bool send(const uint8_t* data, size_t len) {
  Serial.printf("sent: %.*s\n", (int) len, (const char*) data);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  PQ_FS.begin(true);
  if ( !queue.begin("/sf") ) {
    Serial.printf("queue begin failed: %d\n", queue.getLastError());
    while (1) delay(1000);
  }
  queue.setMaxMessages(50, true);

  Serial.printf("PersistentQueue %s: %lu messages waiting\n", PQ_VERSION_STRING, (unsigned long) queue.count());
}

void loop() {
  static uint32_t lastReading = 0;

  if ( millis() - lastReading >= 1000 ) {
    lastReading = millis();
    char msg[32];
    int n = snprintf(msg, sizeof(msg), "reading at %lu ms", (unsigned long) lastReading);
    if ( !queue.enqueue(0, (const uint8_t*) msg, n) ) {
      Serial.printf("enqueue error %d\n", queue.getLastError());
    }
  }

  if ( linkUp() ) {
    uint8_t* data;
    size_t len;
    if ( queue.peek(&data, &len) ) {
      bool sent = send(data, len);
      free(data);   // peek() allocates the buffer - free it
      if ( sent ) queue.drop();
    }
    else if ( queue.getLastError() != PQ_ERROR_QUEUE_EMPTY ) {
      // a message that failed its checks was removed: the next call continues with the next one
      Serial.printf("peek error %d\n", queue.getLastError());
    }
  }

  delay(100);
}
