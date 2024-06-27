#pragma once
#include <Arduino.h>
#include <FS.h>

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
    PQ_ERROR_OTHER
} pqError_t;


// For Arduino IDE - the defines need to be uncommented here. 
// For Platform IO - use build_flags instead (-D PQ_USES_LITTLEFS)
// #define PQ_USES_LITTLEFS

#if !defined(PQ_USES_SPIFFS) && !defined(PQ_USES_LITTLEFS)
#define PQ_USES_SPIFFS
#endif

#define PQ_DEFAULT_MN   0xA55AC0DE

#ifdef PQ_USES_SPIFFS
#include <SPIFFS.h>

#define PQ_FS               SPIFFS
#define PQ_MAX_PREFIX_SIZE      (31-10-1-2)
#define PQ_MAX_FILENAME_SIZE    (31)
#endif

#ifdef PQ_USES_LITTLEFS
#include <LittleFS.h>

#define PQ_FS               LittleFS
#define PQ_MAX_PREFIX_SIZE      (254-10-1-2)
#define PQ_MAX_FILENAME_SIZE    (254)
#endif



#define PQ_MAX_SUBFILENAMES 100 // currently should not be more than double digit 0-99

class PersistentQueue {
    public:
        PersistentQueue(uint32_t magic_num = PQ_DEFAULT_MN, pqDequeueOrder_t dq_order = PQ_DEQUEUE_OLDEST, bool calculate_crc = true);
        ~PersistentQueue();

        bool begin(const char* prefix);
        void end();
        
        bool enqueue(uint32_t name, const uint8_t* data, size_t len);
        bool isQueueEmpty(bool fast_check = true);
        bool dequeue(uint8_t** data, size_t* len, bool fast_check = true);
        bool dequeue(uint8_t* data, size_t len, size_t* actual_len, bool fast_check = true);

        bool purge(bool fast_check = true);

        pqError_t   getLastError() { return m_lastError; }

    private:
        String findNextMessage(bool fast_check = true, pqDequeueOrder_t order = PQ_DEQUEUE_DEFAULT);
        inline bool   checkInitialized();

        uint32_t            m_counter;
        uint32_t            m_current_max;
        uint32_t            m_magic;
        bool                m_calcCRC;
        bool                m_initialized;
        pqDequeueOrder_t    m_order;
        String              m_prefix;
        pqError_t           m_lastError;
        FILE                m_stream;
};
// };

