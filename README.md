# Persistent Queue
### Implementation of a queue that persists messages to flash.
#### Version 1.2.0: 2026-09-29

A queue of binary messages for the ESP32 (arduino-esp32). Each message is a file on SPIFFS or LittleFS, so the queue survives a reset or a power loss. Messages are retrieved oldest first or newest first. The typical use is store-and-forward: a device keeps readings or MQTT messages while it is offline and sends them when the link returns.

## Installation

- Arduino IDE: Library Manager, "PersistentQueue".
- PlatformIO: `lib_deps = https://github.com/arkhipenko/PersistentQueue.git#v1.2.0`, and `lib_ldf_mode = chain+` (the default chain mode compiles both file system libraries).

## File system

SPIFFS is the default. For LittleFS:

- PlatformIO: `build_flags = -D PQ_USES_LITTLEFS`
- Arduino IDE: uncomment `#define PQ_USES_LITTLEFS` in `src/PersistentQueue.h`

`PQ_FS` is the selected file system object. Mount it before `begin()`: `PQ_FS.begin(true)`.

## Quick start

```cpp
#include <PersistentQueue.h>

PersistentQueue queue;                      // oldest first, CRC on

void setup() {
  PQ_FS.begin(true);
  queue.begin("/q");
  const char* msg = "hello";
  queue.enqueue(0, (const uint8_t*) msg, strlen(msg) + 1);   // 0: use the internal counter as the name
}

void loop() {
  uint8_t* data;
  size_t len;
  if ( queue.peek(&data, &len) ) {          // read without removing
    bool sent = publish(data, len);         // your send function
    free(data);
    if ( sent ) queue.drop();               // remove only after a successful send
  }
}
```

See `examples/StoreAndForward` for a complete sketch.

## API

| Call | Description |
|---|---|
| `PersistentQueue(uint32_t magic = PQ_DEFAULT_MN, pqDequeueOrder_t order = PQ_DEQUEUE_OLDEST, bool crc = true)` | `magic` identifies the message type and is stored in every file. `order`: `PQ_DEQUEUE_OLDEST` or `PQ_DEQUEUE_LATEST`. `crc`: store and check a CRC-32 of each message. |
| `bool begin(const char* prefix)` | Opens the queue folder (created on LittleFS). The prefix is 1 to `PQ_MAX_PREFIX_SIZE` characters including the leading `/` (16 on SPIFFS, 48 on LittleFS with the default core settings). Returns false with `PQ_ERROR_INVALID_PREFIX` for a bad prefix or an unmounted file system. |
| `void end()` | Stops the queue. The messages stay on flash. |
| `bool enqueue(uint32_t name, const uint8_t* data, size_t len)` | Stores a message. `name` orders the messages (a counter or a timestamp); 0 uses the internal counter, which continues after a restart. Up to 100 messages can share a name. |
| `uint32_t count(bool fast_check = true)` | Number of messages. |
| `bool isQueueEmpty(bool fast_check = true)` | True when there is no message. False also on an error (see `getLastError()`). |
| `bool dequeue(uint8_t** data, size_t* len, bool fast_check = true)` | Removes the next message into a `malloc` buffer. The caller frees it. |
| `bool dequeue(uint8_t* data, size_t len, size_t* actual_len, bool fast_check = true)` | Removes the next message into a caller buffer. `PQ_ERROR_SMALL_BUFFER` leaves the message and sets `*actual_len` to the needed size (`data` may be NULL with `len` 0 to ask for the size). |
| `bool peek(...)` | As the two `dequeue()` calls, but the message stays in the queue. |
| `bool drop(bool fast_check = true)` | Removes the message returned by the last `peek()`, or the next message when there was no `peek()`. |
| `bool purge(bool fast_check = true)` | Removes all messages of the queue. Other files in the folder are not touched. |
| `void setMaxMessages(uint32_t max, bool drop_oldest = false)` | Limits the queue to `max` messages (0: no limit). When full, `enqueue()` fails with `PQ_ERROR_QUEUE_FULL`, or removes the oldest messages when `drop_oldest` is true. |
| `pqError_t getLastError()` | Result of the last call. |

`fast_check = true` treats every message file in the folder as a message of this queue. `fast_check = false` also checks the magic number, so several queues with different magic numbers can share one folder.

`PQ_VERSION_STRING` ("1.2.0") and `PQ_VERSION` (10200) give the library version.

## Ordering

- `PQ_DEQUEUE_OLDEST` returns the lowest name first, `PQ_DEQUEUE_LATEST` the highest.
- Messages with the same name come out in the order of their subnumbers.
- `peek()` then `drop()` removes the peeked message even if a newer one arrived in between. `dequeue()` between them cancels the peek.

## Delivery and power loss

- `dequeue()` returns true only after the message file is removed. If the file cannot be removed, it returns false and the next call returns the same message.
- `peek()` plus `drop()` gives at-least-once delivery: a reset before `drop()` returns the message again.
- `enqueue()` writes to a temporary file (`~` plus the name) and renames it when the write is complete. A reset during `enqueue()` leaves no partial message. `begin()` removes such temporary files.
- A message file that fails its checks (too short, wrong magic number, bad CRC) cannot be delivered. `dequeue()` and `peek()` remove it and return false with the error. The next call continues with the next message.

## Limits and costs

- No locking. Use one queue object from one task.
- `dequeue()`, `peek()` and `count()` read the queue folder once, `drop()` at most once. `isQueueEmpty()` stops at the first message. `fast_check = false` also reads the first 4 bytes of every file.
- `enqueue()` checks up to 100 file names for a free subnumber. With a limit set, it also reads the folder once.
- There is no size limit other than `setMaxMessages()` and the free space of the file system.

## File format

File name: `<prefix>/<name>-<subnumber>`, for example `/q/1790000000-00` (name as 10 decimal digits, subnumber as 2).

File content: 4-byte magic number, the message, then a 4-byte CRC of the message when CRC is on (little-endian). The CRC uses polynomial 0xEDB88320 and initial value 0xFFFFFFFF without the final inversion. Example: default magic number, message "hi" with its terminating zero:

```
DE C0 5A A5  68 69 00  9B 6C 45 F2
magic        message   CRC
```

## Error codes

| Code | Value | Meaning |
|---|---|---|
| `PQ_ERROR_OK` | 0 | Success |
| `PQ_ERROR_NOT_INITIALIZED` | 1 | `begin()` not called or `end()` called |
| `PQ_ERROR_FILE_OP` | 2 | A file could not be created, written, renamed, read or removed, or it is too short |
| `PQ_ERROR_OUT_OF_SUBNUMBERS` | 3 | 100 messages already use this name |
| `PQ_ERROR_INVALID_PREFIX` | 4 | Bad prefix, or the folder cannot be opened |
| `PQ_ERROR_QUEUE_EMPTY` | 5 | No message |
| `PQ_ERROR_INVALID_MAGIC` | 6 | The next message has another magic number (it was removed) |
| `PQ_ERROR_OUT_OF_MEMORY` | 7 | `malloc` failed |
| `PQ_ERROR_BAD_CRC` | 8 | The CRC does not match (the message was removed) |
| `PQ_ERROR_NULL_POINTER` | 9 | A required pointer is NULL |
| `PQ_ERROR_SMALL_BUFFER` | 10 | The buffer is too small |
| `PQ_ERROR_OTHER` | 11 | Not used |
| `PQ_ERROR_QUEUE_FULL` | 12 | The message limit is reached |

## Upgrading to 1.2.0

- With arduino-esp32 2.x and 3.x, 1.1.0 and older compared only part of the name, so messages could come out in the wrong order, and a prefix of 9 or more characters could make the queue look empty. 1.2.0 orders by the full name. Messages already on flash are read and come out in the correct order.
- `PQ_DEQUEUE_LATEST` now returns the message named 1.
- A bad message file is removed when it is met, instead of blocking the queue.
- `purge()` removes only queue files and keeps the folder. The queue stays usable after it.
- `begin()` returns false for an empty or too long prefix, or an unmounted file system.
- Names from 2^31 up are stored as unsigned numbers. Files written by older versions with such names are still read.
- `PQ_DEQUEUE_DEFAULT` in the constructor now means `PQ_DEQUEUE_OLDEST`.
- `count()` and `isQueueEmpty()` ignore files that are not queue files.

## Version history

- 1.2.0 (2026-09-29): ordering fixed for arduino-esp32 2.x and 3.x, power-loss safe `enqueue()`, bad messages no longer block the queue, `peek()`, `drop()`, `setMaxMessages()`, version macros, examples, host tests and CI.
- 1.1.0: `count()`: number of queued messages. Not released separately.
- 1.0.1: support for IDF 3.x.
- 1.0.0 (2024-06-27): initial release.

## License

BSD 3-Clause. See LICENSE.txt.
