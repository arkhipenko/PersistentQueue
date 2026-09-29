/*
  Two message types in one folder, and the calls the other examples do not show.

  - Two queues share the folder "/shared". Each has its own magic number, and every
    call passes fast_check = false, so each queue sees only its own messages.
  - The alarm queue holds at most 3 messages in reject mode: enqueue() fails with
    PQ_ERROR_QUEUE_FULL when it is full.
  - peek() into a caller buffer, then drop() once the message is handled.
  - isQueueEmpty(), getMaxMessages(), end() and begin() again.

  Uses SPIFFS (the default). For LittleFS see LittleFS_Example.
*/
#include <Arduino.h>
#include <PersistentQueue.h>

#define FULL_CHECK  false             // fast_check = false: check the magic number of every file

PersistentQueue readings(0x52454144); // "READ"
PersistentQueue alarms(0x414C524D);   // "ALRM"

void store(PersistentQueue& q, const char* name, const char* text) {
  if ( q.enqueue(0, (const uint8_t*) text, strlen(text) + 1) ) {
    Serial.printf("%s: stored \"%s\"\n", name, text);
  }
  else if ( q.getLastError() == PQ_ERROR_QUEUE_FULL ) {
    Serial.printf("%s: full (limit %lu), \"%s\" rejected\n", name, (unsigned long) q.getMaxMessages(), text);
  }
  else {
    Serial.printf("%s: enqueue error %d\n", name, q.getLastError());
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nPersistentQueue: two queues in one folder\n");

  PQ_FS.begin(true);
  if ( !readings.begin("/shared") || !alarms.begin("/shared") ) {
    Serial.printf("begin failed: %d / %d\n", readings.getLastError(), alarms.getLastError());
    while (1) delay(1000);
  }

  // start clean: with the full check each purge() removes only its own messages
  readings.purge(FULL_CHECK);
  alarms.purge(FULL_CHECK);

  alarms.setMaxMessages(3);           // reject mode: enqueue() fails while 3 alarms are waiting

  store(readings, "readings", "temperature 21.5");
  store(alarms, "alarms", "door open");
  store(readings, "readings", "temperature 21.7");
  store(alarms, "alarms", "door closed");
  store(alarms, "alarms", "battery low");
  store(alarms, "alarms", "battery critical");    // rejected: the limit is 3

  Serial.printf("\nreadings: %lu, alarms: %lu (limit %lu)\n\n",
                (unsigned long) readings.count(FULL_CHECK),
                (unsigned long) alarms.count(FULL_CHECK),
                (unsigned long) alarms.getMaxMessages());

  // peek() into a caller buffer, drop() after the message is handled
  char buf[64];
  size_t len;
  while ( !alarms.isQueueEmpty(FULL_CHECK) ) {
    if ( !alarms.peek((uint8_t*) buf, sizeof(buf), &len, FULL_CHECK) ) {
      Serial.printf("alarms: peek error %d\n", alarms.getLastError());
      break;
    }
    Serial.printf("alarms: handling \"%s\" (%u bytes)\n", buf, (unsigned int) len);
    alarms.drop(FULL_CHECK);
  }

  // end() stops a queue; its messages stay on flash until begin() is called again
  readings.end();
  uint32_t n = readings.count(FULL_CHECK);        // call first: argument order is not defined
  Serial.printf("\nreadings after end(): count() = %lu, error %d (not initialized)\n",
                (unsigned long) n, readings.getLastError());
  readings.begin("/shared");
  Serial.printf("readings after begin(): %lu messages waiting\n\n", (unsigned long) readings.count(FULL_CHECK));

  while ( readings.dequeue((uint8_t*) buf, sizeof(buf), &len, FULL_CHECK) ) {
    Serial.printf("readings: \"%s\"\n", buf);
  }

  Serial.printf("\nreadings empty: %s, alarms empty: %s\n",
                readings.isQueueEmpty(FULL_CHECK) ? "yes" : "no",
                alarms.isQueueEmpty(FULL_CHECK) ? "yes" : "no");
}

void loop() {
  delay(1000);
}
