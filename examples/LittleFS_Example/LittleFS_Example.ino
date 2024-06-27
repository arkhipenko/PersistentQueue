#include <Arduino.h>
#include <PersistentQueue.h>

// Uncomment 
// #define PQ_USES_LITTLEFS
// in PersistentQueue.h file of the library to make it work with Arduino IDE


const char* tst1 = "This is a test message 1";
const char* tst2 = "This is a test message number 2";
const char* tst3 = "This is a test message number 3 which is longer than message 2";

#define BUF_LEN 256 //59

char  buf[BUF_LEN];
size_t len;

PersistentQueue fq1;
PersistentQueue fq2(0x12345678, PQ_DEQUEUE_LATEST, true);

void setup (void)
{

  Serial.begin (115200);
  delay(1000);

  Serial.println ("\n\nPersistent Queue Testing\n\n");
  LittleFS.begin(true);

  fq1.begin("/fq1");
  fq2.begin("/fq2/");

}  // end of setup


void printFileContentsHex(File file);
void listSPIFFSContents(String folderPath) {
  File root = LittleFS.open(folderPath);
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      Serial.print("FILE: ");
      Serial.print(file.path());
      Serial.print(" SIZE: ");
      Serial.println(file.size());

      Serial.println("CONTENT:");
      printFileContentsHex(file);
    } else {
      Serial.print("DIR : ");
      Serial.println(file.name());
    }
    file = root.openNextFile();
  }
}

void printFileContentsHex(File file) {
  file.seek(0); // Go to the beginning of the file
  size_t fileSize = file.size();
  uint8_t buffer[16]; // Buffer to hold file data
  size_t bytesRead;

  while (fileSize > 0) {
    bytesRead = file.read(buffer, sizeof(buffer)); // Read up to 16 bytes
    fileSize -= bytesRead;

    // Print the hex values
    for (size_t i = 0; i < bytesRead; ++i) {
      if (buffer[i] < 16) {
        Serial.print("0"); // Print leading zero for single hex digits
      }
      Serial.print(buffer[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
  }
}

/**
 * @brief Main Arduino loop method
 * 
 */
void loop (void)
{
  Serial.println("\n\n\nStoring messages");
  Serial.println("BEFORE:");
  listSPIFFSContents("/");
  Serial.println("================================");

  fq1.enqueue(0, (const uint8_t *)tst1, strlen(tst1)+1);
  fq1.enqueue(0, (const uint8_t *)tst2, strlen(tst2)+1);
  fq1.enqueue(0, (const uint8_t *)tst3, strlen(tst3)+1);
  Serial.println("AFTER:");
  listSPIFFSContents("/");
  Serial.println("================================");

  fq2.enqueue(millis(), (const uint8_t *)tst1, strlen(tst1)+1);
  delay(10);
  fq2.enqueue(millis(), (const uint8_t *)tst2, strlen(tst2)+1);
  delay(20);
  fq2.enqueue(millis(), (const uint8_t *)tst3, strlen(tst3)+1);
  Serial.println("AFTER 2:");
  listSPIFFSContents("/");
  Serial.println("================================");
  
  delay(5000);

  Serial.println("\n\nRestoring messages");

  uint8_t* p;
  size_t   l;
  for (int i=0; i<3; i++) {
    if ( fq1.dequeue(&p, &l) ) {
      Serial.printf("Retrieved from fq1: %s (%d)\n", p, l);
      free(p);
    }
    else {
      Serial.printf("Error retrieving msg #%d fq1: %d\n", i, fq1.getLastError());
    }
  }
  Serial.println("AFTER:");
  listSPIFFSContents("/");
  Serial.println("================================");
  
  for (int i=0; i<3; i++) {
    if ( fq2.dequeue((uint8_t*)buf, BUF_LEN, &l) ) {
      Serial.printf("Retrieved from fq2: %s (%d)\n", buf, l);
    }
    else {
      Serial.printf("Error retrieving msg #%d fq2: %d (%d)\n", i, fq2.getLastError(), l);
    }
  }
  Serial.println("AFTER 2:");
  listSPIFFSContents("/");
  Serial.println("================================");
  Serial.println("\n\n");

  delay(30000);

  Serial.println("Purging messages:");
  fq1.purge();
  listSPIFFSContents("/");
  Serial.println("================================");
  fq2.purge();
  listSPIFFSContents("/");
  Serial.println("================================");
  Serial.println("\n\n");

  while(1) ;
}

