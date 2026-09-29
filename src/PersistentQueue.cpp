#include <Arduino.h>
#include "PersistentQueue.h"


static const uint32_t pq_crc32tab[16] = {
   0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190,
   0x6b6b51f4, 0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344,
   0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278,
   0xbdbdf21c
};


/**
 * @brief calculates CRC32 value for a given buffer/length
 *
 * @param data - pointer to the array if usigned bytes
 * @param length - length of the array
 * @param crc previous value for incremental computation, 0xffffffff initially
 * @return uint32_t - calculated CRC32
 */
static uint32_t pq_crc32(const void *data, size_t length, uint32_t crc = 0xffffffff)
{
   const unsigned char *buf = (const unsigned char *)data;
   size_t i;

   for (i = 0; i < length; ++i)
   {
      crc ^= buf[i];
      crc = pq_crc32tab[crc & 0x0f] ^ (crc >> 4);
      crc = pq_crc32tab[crc & 0x0f] ^ (crc >> 4);
   }

   // return value suitable for passing in next time, for final value invert it
   return crc/* ^ 0xffffffff*/;
}


/**
 * @brief Returns the file name part of a path
 *          IDF 3.x File::name() returns the full path, IDF 4.x and above just the name
 */
static const char* pq_baseName(const char* path) {
    const char* p = strrchr(path, '/');
    return p ? p + 1 : path;
}


/**
 * @brief Returns the full path of a folder entry
 *          IDF 3.x cores (arduino-esp32 1.x) have no File::path(), their File::name() is the full path
 */
static const char* pq_fullPath(File& file) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 2
    return file.path();
#else
    return file.name();
#endif
}


/**
 * @brief Parses a message file name
 *          1234567890-99 is written by 1.2.0 and later.
 *          1.1.0 and older wrote names from 2^31 up as negative numbers
 *          (-1294967296-00 for 3000000000). They are mapped back to the original number.
 *
 * @param name - file name without the path
 * @param number - message name number
 * @param sub - subnumber 0-99
 * @return true - this is a message file name
 * @return false - any other file name
 */
static bool pq_parseName(const char* name, uint32_t* number, uint8_t* sub) {
    size_t n = strlen(name);
    if ( n < PQ_NAME_LEN || n > PQ_NAME_LEN + 1 ) return false;

    const char* s = name + n - 3;   // "-99"
    if ( s[0] != '-' || s[1] < '0' || s[1] > '9' || s[2] < '0' || s[2] > '9' ) return false;

    bool negative = ( name[0] == '-' );
    const char* d = negative ? name + 1 : name;
    size_t digits = s - d;
    if ( negative ? ( digits != 9 && digits != 10 ) : ( digits != 10 ) ) return false;

    uint64_t v = 0;
    for ( ; d < s; d++ ) {
        if ( *d < '0' || *d > '9' ) return false;
        v = v * 10 + (uint64_t) (*d - '0');
    }

    if ( negative ) {
        if ( v == 0 || v > 2147483648ULL ) return false;
        v = 4294967296ULL - v;
    }
    else if ( v > UINT32_MAX ) {
        return false;
    }

    *number = (uint32_t) v;
    *sub = (uint8_t) ( (s[1] - '0') * 10 + (s[2] - '0') );
    return true;
}

// namespace PQ {

/**
 * @brief Construct a new Persistent Queue:: Persistent Queue object
 *
 * @param magic_num - message type ID
 * @param dq_order - desired retrival order: latest message first or oldest message first
 *                   (PQ_DEQUEUE_DEFAULT is taken as PQ_DEQUEUE_OLDEST)
 * @param calculate_crc - calculate and store CRC within the message file for consistency checking
 */
PersistentQueue::PersistentQueue(uint32_t magic_num, pqDequeueOrder_t dq_order, bool calculate_crc) {
    m_magic = magic_num;
    m_calcCRC = calculate_crc;
    m_order = ( dq_order == PQ_DEQUEUE_LATEST ) ? PQ_DEQUEUE_LATEST : PQ_DEQUEUE_OLDEST;
    m_counter = 1;
    m_maxMessages = 0;
    m_dropOldest = false;
    m_initialized = false;
    m_lastError = PQ_ERROR_OK;
}

/**
 * @brief Destroy the Persistent Queue:: Persistent Queue object
 *
 */
PersistentQueue::~PersistentQueue() {
}


/**
 * @brief Initializes the PQ object and provides storage prefix
 *          prefix should start with an '/' and should not end with an '/'
 *          The file system has to be mounted before this call.
 *
 * @param prefix - queue folder, 1 to PQ_MAX_PREFIX_SIZE characters including the leading '/'
 * @return true - queue is ready
 * @return false - PQ_ERROR_INVALID_PREFIX: empty or too long prefix, or the folder cannot be opened
 */
bool PersistentQueue::begin(const char* prefix) {
    m_initialized = false;
    m_peeked = "";

    m_prefix = prefix ? prefix : "";
    if ( m_prefix.length()>0 && !m_prefix.startsWith("/") ) m_prefix = "/" + m_prefix;

    while ( m_prefix.endsWith("/") ) {
        m_prefix.remove(m_prefix.length() - 1);
    }

    if ( m_prefix.length() < 2 || m_prefix.length() > (unsigned int) PQ_MAX_PREFIX_SIZE ) {
        m_lastError = PQ_ERROR_INVALID_PREFIX;
        return false;
    }

#ifdef PQ_USES_LITTLEFS
    if ( !PQ_FS.exists(m_prefix) ) PQ_FS.mkdir(m_prefix);
#endif

    File root = PQ_FS.open(m_prefix);
    bool isFolder = root && root.isDirectory();
    root.close();
    if ( !isFolder ) {
        m_lastError = PQ_ERROR_INVALID_PREFIX;
        return false;
    }

    m_initialized = true;
    removeTempFiles();

    // reset the internal counter to the latest number in case of restart after power failure
    uint32_t latest = 0;
    if ( scan(false, PQ_DEQUEUE_LATEST, NULL, &latest) > 0 ) {
        m_counter = latest + 1;
        if ( m_counter == 0 ) m_counter = 1;
    }

    m_lastError = PQ_ERROR_OK;
    return true;
}

/**
 * @brief Stop queue processing
 *
 */
void PersistentQueue::end() {
    m_lastError = PQ_ERROR_OK;
    m_initialized = false;
    m_peeked = "";
}

bool PersistentQueue::checkInitialized() {
    if ( !m_initialized) {
        m_lastError = PQ_ERROR_NOT_INITIALIZED;
        return false;
    }
    return true;
}


/**
 * @brief Limit the number of messages in the queue
 *
 * @param max_messages - maximum number of messages with this queue's magic number, 0 - no limit
 * @param drop_oldest - when full: true - remove the oldest messages to make room,
 *                      false - enqueue fails with PQ_ERROR_QUEUE_FULL
 */
void PersistentQueue::setMaxMessages(uint32_t max_messages, bool drop_oldest) {
    m_maxMessages = max_messages;
    m_dropOldest = drop_oldest;
}


/**
 * @brief Store message on the queue
 *
 * @param name - 32 bit number to be used to name the queue file. 0 if internal counter to be used
 *               developer to supply increasing numbers - the messages will be sorted by the name.
 *               if the same number is already used, a subnumber 0-99 will be used.
 *               messages with the same name are retrieved in the order of their subnumbers
 *               messages are stored in:  /<prefix>/1234567890-99 format
 *               the message is written to /<prefix>/~1234567890-99 first and renamed when complete
 * @param data - message, may be NULL if len is 0
 * @param len - message length
 * @return true - message stored
 * @return false - check getLastError()
 */
bool PersistentQueue::enqueue(uint32_t name, const uint8_t* data, size_t len) {
    if ( !checkInitialized()) return false;

    if ( data == NULL && len > 0 ) {
        m_lastError = PQ_ERROR_NULL_POINTER;
        return false;
    }

    if ( m_maxMessages > 0 && !makeRoom() ) return false;

    // if zero is provided as a name number - use internal counter
    if ( name == 0 ) {
        name = m_counter++;
        if ( m_counter == 0 ) m_counter = 1;
    }

    char fn[PQ_MAX_FILENAME_SIZE+1];
    char tn[PQ_MAX_FILENAME_SIZE+1];

    for (uint8_t i=0; i<PQ_MAX_SUBFILENAMES; i++) {
        //       0123456789012345678
        //      '/q/1234567890-00
        snprintf(fn, sizeof(fn), "%s/%010lu-%02u", m_prefix.c_str(), (unsigned long) name, (unsigned int) i);
        if ( PQ_FS.exists(fn) ) continue;

        snprintf(tn, sizeof(tn), "%s/~%010lu-%02u", m_prefix.c_str(), (unsigned long) name, (unsigned int) i);
        File F = PQ_FS.open(tn, "w");
        if ( !F ) {
            m_lastError = PQ_ERROR_FILE_OP;
            return false;
        }

        size_t expected = sizeof(uint32_t) + len;

        //  write magic number first
        bool ok = ( F.write((const uint8_t*) &m_magic, sizeof(uint32_t)) == sizeof(uint32_t) );

        //  write message
        if ( ok && len > 0 ) ok = ( F.write(data, len) == len );

        //  write crc if requested
        if ( ok && m_calcCRC ) {
            uint32_t crc32 = pq_crc32(data, len);
            ok = ( F.write((const uint8_t*) &crc32, sizeof(uint32_t)) == sizeof(uint32_t) );
            expected += sizeof(uint32_t);
        }

        //  a full file system may only show when the buffered data is written out
        if ( ok ) {
            F.flush();
            ok = ( F.size() == expected );
        }
        F.close();

        if ( ok ) ok = PQ_FS.rename(tn, fn);
        if ( !ok ) {
            PQ_FS.remove(tn);
            m_lastError = PQ_ERROR_FILE_OP;
            return false;
        }

        m_lastError = PQ_ERROR_OK;
        return true;
    }
    m_lastError = PQ_ERROR_OUT_OF_SUBNUMBERS;
    return false;
}


/**
 * @brief Check if queue is empty
 *
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true - the queue is empty
 * @return false - queue has elements (or an error: check getLastError())
 */
bool PersistentQueue::isQueueEmpty(bool fast_check) {
    if ( !checkInitialized()) return false;

    uint32_t n = scan(fast_check, m_order, NULL, NULL, 1);
    if ( m_lastError != PQ_ERROR_OK ) return false;
    return n == 0;
}


/**
 * @brief Count the queued messages (backlog depth) by enumerating the store.
 *
 * @param fast_check - if false, only count files whose magic number matches.
 * @return uint32_t - number of queued messages; 0 if not initialized / no store.
 */
uint32_t PersistentQueue::count(bool fast_check) {
    if ( !checkInitialized() ) return 0;

    return scan(fast_check, m_order, NULL, NULL);
}


/**
 * @brief Dequeue next message and place it into the provided buffer
 *          If buffer is too small, the actual length required could be checked after the call
 *
 * @param data - pointer to the buffer where the content of the message should be placed
 * @param len - available length of the buffer
 * @param actual_len - pointer to the variable to place actual length of the data read
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true - message retrieved successfully
 * @return false - message was not retrieved - check if buffer provided was large enough
 */
bool PersistentQueue::dequeue(uint8_t* data, size_t len, size_t* actual_len, bool fast_check) {
    return retrieve(false, NULL, data, len, actual_len, fast_check, true);
}


/**
 * @brief Dequeue next message, allocate memory for the contents
 *
 * @param data - pointer to a pointer - where the address of the buffer will be stored
 *               the buffer is allocated with malloc() - the caller has to free() it
 * @param len - actual length of the dequeued message
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true
 * @return false
 */
bool PersistentQueue::dequeue(uint8_t** data, size_t* len, bool fast_check) {
    return retrieve(true, data, NULL, 0, len, fast_check, true);
}


/**
 * @brief Read next message into the provided buffer without removing it from the queue
 *          Call drop() to remove it once it has been processed
 *
 * @param data - pointer to the buffer where the content of the message should be placed
 * @param len - available length of the buffer
 * @param actual_len - pointer to the variable to place actual length of the data read
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true - message retrieved successfully
 * @return false - message was not retrieved - check getLastError()
 */
bool PersistentQueue::peek(uint8_t* data, size_t len, size_t* actual_len, bool fast_check) {
    return retrieve(false, NULL, data, len, actual_len, fast_check, false);
}


/**
 * @brief Read next message into allocated memory without removing it from the queue
 *          Call drop() to remove it once it has been processed
 *
 * @param data - pointer to a pointer - where the address of the buffer will be stored
 *               the buffer is allocated with malloc() - the caller has to free() it
 * @param len - actual length of the message
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true
 * @return false
 */
bool PersistentQueue::peek(uint8_t** data, size_t* len, bool fast_check) {
    return retrieve(true, data, NULL, 0, len, fast_check, false);
}


/**
 * @brief Remove the message returned by the last peek(), or the next message if there was no peek()
 *
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true - message removed
 * @return false - PQ_ERROR_QUEUE_EMPTY or PQ_ERROR_FILE_OP
 */
bool PersistentQueue::drop(bool fast_check) {
    if ( !checkInitialized()) return false;

    String fp = m_peeked;
    m_peeked = "";

    if ( fp.length() == 0 ) {
        scan(fast_check, m_order, &fp, NULL);
        if ( m_lastError != PQ_ERROR_OK ) return false;
        if ( fp.length() == 0 ) {
            m_lastError = PQ_ERROR_QUEUE_EMPTY;
            return false;
        }
    }

    if ( !PQ_FS.remove(fp) ) {
        m_lastError = PQ_ERROR_FILE_OP;
        return false;
    }

    m_lastError = PQ_ERROR_OK;
    return true;
}


/**
 * @brief Common part of dequeue() and peek()
 *          A message file that fails the checks can never be delivered.
 *          It is removed, so the next call returns the next message.
 *
 * @param allocate - true: allocate the buffer and store its address in alloc_data
 * @param alloc_data - where the address of the allocated buffer is stored
 * @param data - caller buffer (allocate = false)
 * @param len - length of the caller buffer
 * @param actual_len - where the message length is stored
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @param remove - true: remove the message after a successful read
 * @return true - message retrieved (and removed if requested)
 * @return false - check getLastError()
 */
bool PersistentQueue::retrieve(bool allocate, uint8_t** alloc_data, uint8_t* data, size_t len, size_t* actual_len, bool fast_check, bool remove) {
    if ( !checkInitialized()) return false;

    if ( actual_len == NULL || ( allocate && alloc_data == NULL ) ) {
        m_lastError = PQ_ERROR_NULL_POINTER;
        return false;
    }

    //  dequeue cancels a pending peek
    if ( remove ) m_peeked = "";

    String fp;
    scan(fast_check, m_order, &fp, NULL);
    if ( m_lastError != PQ_ERROR_OK ) return false;

    if ( fp.length() == 0 ) {
        m_lastError = PQ_ERROR_QUEUE_EMPTY;
        return false;
    }

    File F = PQ_FS.open(fp, "r");
    if ( !F ) {
        m_lastError = PQ_ERROR_FILE_OP;
        return false;
    }

    size_t expected_min_len = sizeof(uint32_t);
    if ( m_calcCRC ) expected_min_len += sizeof(uint32_t);

    size_t fsz = F.size();
    if ( fsz < expected_min_len ) {
        F.close();
        PQ_FS.remove(fp);
        m_lastError = PQ_ERROR_FILE_OP;
        return false;
    }

    fsz -= expected_min_len;

    // check magic number
    uint32_t mn = 0;
    if ( F.read((uint8_t*) &mn, sizeof(uint32_t)) != sizeof(uint32_t) ) {
        F.close();
        PQ_FS.remove(fp);
        m_lastError = PQ_ERROR_FILE_OP;
        return false;
    }
    if ( mn != m_magic ) {
        F.close();
        PQ_FS.remove(fp);
        m_lastError = PQ_ERROR_INVALID_MAGIC;
        return false;
    }

    uint8_t* p = data;
    if ( allocate ) {
        p = (uint8_t*) malloc(fsz > 0 ? fsz : 1);
        if ( p == NULL ) {
            m_lastError = PQ_ERROR_OUT_OF_MEMORY;
            F.close();
            return false;
        }
    }
    else {
        *actual_len = fsz;
        if ( len < fsz ) {
            m_lastError = PQ_ERROR_SMALL_BUFFER;
            F.close();
            return false;
        }
        if ( data == NULL ) {
            m_lastError = PQ_ERROR_NULL_POINTER;
            F.close();
            return false;
        }
        memset(data, 0, len);
    }

    pqError_t err = PQ_ERROR_OK;
    if ( fsz > 0 && F.read(p, fsz) != fsz ) err = PQ_ERROR_FILE_OP;

    if ( err == PQ_ERROR_OK && m_calcCRC ) {
        uint32_t crc = 0;
        if ( F.read((uint8_t*) &crc, sizeof(uint32_t)) != sizeof(uint32_t) ) err = PQ_ERROR_FILE_OP;
        else if ( crc != pq_crc32(p, fsz) ) err = PQ_ERROR_BAD_CRC;
    }
    F.close();

    if ( err != PQ_ERROR_OK ) {
        if ( allocate ) free(p);
        PQ_FS.remove(fp);
        m_lastError = err;
        return false;
    }

    if ( remove && !PQ_FS.remove(fp) ) {
        //  not removed - not delivered either: the next call returns the same message
        if ( allocate ) free(p);
        m_lastError = PQ_ERROR_FILE_OP;
        return false;
    }

    if ( allocate ) *alloc_data = p;
    *actual_len = fsz;
    if ( !remove ) m_peeked = fp;

    m_lastError = PQ_ERROR_OK;
    return true;
}


/**
 * @brief Classify one entry of the queue folder
 *
 * @param file - the folder entry
 * @param check_magic - true: a file with another magic number is not a message of this queue
 * @param number - name number of a message or temp file
 * @param sub - subnumber of a message or temp file
 * @return PQ_FILE_MESSAGE, PQ_FILE_TEMP (unfinished enqueue) or PQ_FILE_OTHER
 */
PersistentQueue::pqFileKind_t PersistentQueue::classify(File& file, bool check_magic, uint32_t* number, uint8_t* sub) {
    if ( file.isDirectory() ) return PQ_FILE_OTHER;

    //  SPIFFS lists the files of nested "folders" as well: only files directly in the queue folder count
    const char* path = pq_fullPath(file);
    const char* name = pq_baseName(path);
    if ( (size_t) (name - path) != m_prefix.length() + 1 || strncmp(path, m_prefix.c_str(), m_prefix.length()) != 0 ) return PQ_FILE_OTHER;

    pqFileKind_t kind = PQ_FILE_MESSAGE;
    if ( name[0] == '~' ) {
        kind = PQ_FILE_TEMP;
        name++;
    }
    if ( !pq_parseName(name, number, sub) ) return PQ_FILE_OTHER;

    // if we are asked to check the message type - do it!
    if ( check_magic ) {
        uint32_t mn = 0;
        if ( file.read((uint8_t*) &mn, sizeof(uint32_t)) != sizeof(uint32_t) || mn != m_magic ) return PQ_FILE_OTHER;
    }
    return kind;
}


/**
 * @brief One pass over the queue folder: count the messages and find the next one
 *
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @param order - PQ_DEQUEUE_OLDEST: select the lowest name, PQ_DEQUEUE_LATEST: the highest
 * @param next - if not NULL: path of the selected message, empty if there is none
 * @param next_number - if not NULL: name number of the selected message (unchanged if there is none)
 * @param stop_after - stop after this many messages, 0 - scan the whole folder
 * @return uint32_t - number of messages; 0 with PQ_ERROR_INVALID_PREFIX if the folder cannot be opened
 */
uint32_t PersistentQueue::scan(bool fast_check, pqDequeueOrder_t order, String* next, uint32_t* next_number, uint32_t stop_after) {
    if ( next ) *next = "";

    File root = PQ_FS.open(m_prefix);
    if ( !root || !root.isDirectory() ) {
        m_lastError = PQ_ERROR_INVALID_PREFIX;
        return 0;
    }
    m_lastError = PQ_ERROR_OK;

    bool select = ( next != NULL || next_number != NULL );
    bool found = false;
    uint64_t best = 0;
    String bestName;
    uint32_t n = 0;

    for ( File file = root.openNextFile(); file; file = root.openNextFile() ) {
        uint32_t number;
        uint8_t sub;
        if ( classify(file, !fast_check, &number, &sub) == PQ_FILE_MESSAGE ) {
            n++;
            //  messages with the same name are ordered by their subnumber
            uint64_t key = ( (uint64_t) number << 8 ) | sub;
            if ( select && ( !found || ( order == PQ_DEQUEUE_LATEST ? key > best : key < best ) ) ) {
                found = true;
                best = key;
                bestName = pq_baseName(file.name());
            }
        }
        file.close();
        if ( stop_after > 0 && n >= stop_after ) break;
    }

    if ( found ) {
        if ( next ) *next = m_prefix + '/' + bestName;
        if ( next_number ) *next_number = (uint32_t) ( best >> 8 );
    }
    return n;
}


/**
 * @brief Apply the message limit before a new message is stored
 *
 * @return true - there is room for one more message
 * @return false - PQ_ERROR_QUEUE_FULL, PQ_ERROR_INVALID_PREFIX or PQ_ERROR_FILE_OP
 */
bool PersistentQueue::makeRoom() {
    String oldest;
    uint32_t n = scan(false, PQ_DEQUEUE_OLDEST, &oldest, NULL);
    if ( m_lastError != PQ_ERROR_OK ) return false;

    while ( n >= m_maxMessages ) {
        if ( !m_dropOldest ) {
            m_lastError = PQ_ERROR_QUEUE_FULL;
            return false;
        }
        if ( !PQ_FS.remove(oldest) ) {
            m_lastError = PQ_ERROR_FILE_OP;
            return false;
        }

        n--;
        if ( n >= m_maxMessages ) {
            n = scan(false, PQ_DEQUEUE_OLDEST, &oldest, NULL);
            if ( m_lastError != PQ_ERROR_OK ) return false;
        }
    }
    return true;
}


/**
 * @brief Remove the temp files of enqueue() calls interrupted by a reset
 */
void PersistentQueue::removeTempFiles() {
    File root = PQ_FS.open(m_prefix);
    if ( !root || !root.isDirectory() ) return;

    for ( File file = root.openNextFile(); file; file = root.openNextFile() ) {
        uint32_t number;
        uint8_t sub;
        pqFileKind_t kind = classify(file, false, &number, &sub);
        String fp = m_prefix + '/' + pq_baseName(file.name());
        file.close();
        if ( kind == PQ_FILE_TEMP ) PQ_FS.remove(fp);
    }
}


/**
 * @brief Deletes all messages of this queue from the queue folder
 *          Files that are not queue files are not touched. The folder stays.
 *
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true - all messages deleted
 * @return false - some messages were not deleted
 */
bool PersistentQueue::purge(bool fast_check) {
    if ( !checkInitialized()) return false;

    m_peeked = "";
    bool result = true;
    uint32_t removed;

    //  repeat until nothing is left: removing files may make the folder iteration skip an entry
    do {
        File root = PQ_FS.open(m_prefix);
        if ( !root || !root.isDirectory() ) {
            m_lastError = PQ_ERROR_INVALID_PREFIX;
            return false;
        }

        removed = 0;
        for ( File file = root.openNextFile(); file; file = root.openNextFile() ) {
            uint32_t number;
            uint8_t sub;
            pqFileKind_t kind = classify(file, !fast_check, &number, &sub);
            String fp = m_prefix + '/' + pq_baseName(file.name());
            file.close();

            if ( kind == PQ_FILE_OTHER ) continue;
            if ( PQ_FS.remove(fp) ) removed++;
            else result = false;
        }
    } while ( removed > 0 );

    m_lastError = result ? PQ_ERROR_OK : PQ_ERROR_FILE_OP;
    return result;
}


// }
