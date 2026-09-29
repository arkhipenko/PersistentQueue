#pragma once
#include <Arduino.h>
#include <FS.h>

#define PQ_VERSION_STRING   "1.2.0"
#define PQ_VERSION          10200   // major * 10000 + minor * 100 + patch

// namespace PQ {

typedef enum {
    PQ_DEQUEUE_DEFAULT,
    PQ_DEQUEUE_OLDEST,
    PQ_DEQUEUE_LATEST,
} pqDequeueOrder_t;

typedef enum {
    PQ_ERROR_OK = 0,
    PQ_ERROR_NOT_INITIALIZED,
    PQ_ERROR_FILE_OP,
    PQ_ERROR_OUT_OF_SUBNUMBERS,
    PQ_ERROR_INVALID_PREFIX,
    PQ_ERROR_QUEUE_EMPTY,
    PQ_ERROR_INVALID_MAGIC,
    PQ_ERROR_OUT_OF_MEMORY,
    PQ_ERROR_BAD_CRC,
    PQ_ERROR_NULL_POINTER,
    PQ_ERROR_SMALL_BUFFER,
    PQ_ERROR_OTHER,
    PQ_ERROR_QUEUE_FULL,
} pqError_t;


// For Arduino IDE - the defines need to be uncommented here.
// For Platform IO - use build_flags instead (-D PQ_USES_LITTLEFS)
// #define PQ_USES_LITTLEFS

#if defined(PQ_USES_SPIFFS) && defined(PQ_USES_LITTLEFS)
#error "PersistentQueue: define only one of PQ_USES_SPIFFS and PQ_USES_LITTLEFS"
#endif

#if !defined(PQ_USES_SPIFFS) && !defined(PQ_USES_LITTLEFS)
#define PQ_USES_SPIFFS
#endif

#define PQ_DEFAULT_MN   0xA55AC0DE

//  PQ_MAX_FILENAME_SIZE - longest file path (without the terminating zero) the file system accepts
#ifdef PQ_USES_SPIFFS
#include <SPIFFS.h>

#define PQ_FS               SPIFFS
#ifdef CONFIG_SPIFFS_OBJ_NAME_LEN
#define PQ_MAX_FILENAME_SIZE    (CONFIG_SPIFFS_OBJ_NAME_LEN - 1)
#else
#define PQ_MAX_FILENAME_SIZE    (31)
#endif
#endif

#ifdef PQ_USES_LITTLEFS
#include <LittleFS.h>

#define PQ_FS               LittleFS
#ifdef CONFIG_LITTLEFS_OBJ_NAME_LEN
#define PQ_MAX_FILENAME_SIZE    (CONFIG_LITTLEFS_OBJ_NAME_LEN - 1)
#else
#define PQ_MAX_FILENAME_SIZE    (63)
#endif
#endif

#define PQ_NAME_LEN             13      // message file name: 1234567890-99
//  longest prefix: room for the '/' separator, the file name and the '~' temp file marker
#define PQ_MAX_PREFIX_SIZE      (PQ_MAX_FILENAME_SIZE - PQ_NAME_LEN - 2)

#define PQ_MAX_SUBFILENAMES 100 // currently should not be more than double digit 0-99

class PersistentQueue {
    public:
        PersistentQueue(uint32_t magic_num = PQ_DEFAULT_MN, pqDequeueOrder_t dq_order = PQ_DEQUEUE_OLDEST, bool calculate_crc = true);
        ~PersistentQueue();

        bool begin(const char* prefix);
        void end();

        bool enqueue(uint32_t name, const uint8_t* data, size_t len);
        bool isQueueEmpty(bool fast_check = true);
        uint32_t count(bool fast_check = true);     //  number of queued messages (backlog depth)
        bool dequeue(uint8_t** data, size_t* len, bool fast_check = true);
        bool dequeue(uint8_t* data, size_t len, size_t* actual_len, bool fast_check = true);
        bool peek(uint8_t** data, size_t* len, bool fast_check = true);
        bool peek(uint8_t* data, size_t len, size_t* actual_len, bool fast_check = true);
        bool drop(bool fast_check = true);

        bool purge(bool fast_check = true);

        void        setMaxMessages(uint32_t max_messages, bool drop_oldest = false);
        uint32_t    getMaxMessages() { return m_maxMessages; }

        pqError_t   getLastError() { return m_lastError; }

    private:
        typedef enum {
            PQ_FILE_OTHER,
            PQ_FILE_MESSAGE,
            PQ_FILE_TEMP,
        } pqFileKind_t;

        pqFileKind_t classify(File& file, bool check_magic, uint32_t* number, uint8_t* sub);
        uint32_t     scan(bool fast_check, pqDequeueOrder_t order, String* next, uint32_t* next_number, uint32_t stop_after = 0);
        bool         retrieve(bool allocate, uint8_t** alloc_data, uint8_t* data, size_t len, size_t* actual_len, bool fast_check, bool remove);
        bool         makeRoom();
        void         removeTempFiles();
        bool         checkInitialized();

        uint32_t            m_counter;
        uint32_t            m_magic;
        uint32_t            m_maxMessages;
        bool                m_dropOldest;
        bool                m_calcCRC;
        bool                m_initialized;
        pqDequeueOrder_t    m_order;
        String              m_prefix;
        String              m_peeked;
        pqError_t           m_lastError;
};
// };

