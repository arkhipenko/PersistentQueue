#include <Arduino.h>
#include "PersistentQueue.h"


static const unsigned int pq_crc32tab[16] = {
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
uint32_t pq_crc32(const void *data, unsigned int length, uint32_t crc = 0xffffffff)
{
   const unsigned char *buf = (const unsigned char *)data;
   unsigned int i;

   for (i = 0; i < length; ++i)
   {
      crc ^= buf[i];
      crc = pq_crc32tab[crc & 0x0f] ^ (crc >> 4);
      crc = pq_crc32tab[crc & 0x0f] ^ (crc >> 4);
   }

   // return value suitable for passing in next time, for final value invert it
   return crc/* ^ 0xffffffff*/;
}

// namespace PQ {

/**
 * @brief Construct a new Persistent Queue:: Persistent Queue object
 * 
 * @param magic_num - message type ID
 * @param dq_order - desired retrival order: latest message first or oldest message first
 * @param calculate_crc - calculate and store CRC within the message file for consistency checking
 */
PersistentQueue::PersistentQueue(uint32_t magic_num, pqDequeueOrder_t dq_order, bool calculate_crc) {
    m_magic = magic_num;
    m_calcCRC = calculate_crc;
    m_order = dq_order;
    m_counter = 1;
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
 * 
 * @param prefix 
 * @return true 
 * @return false 
 */
bool PersistentQueue::begin(const char* prefix) {
    m_prefix = prefix;
    if ( m_prefix.length()>0 && !m_prefix.startsWith("/") ) m_prefix = "/" + m_prefix;

    while ( m_prefix.endsWith("/") ) {
        m_prefix.remove(m_prefix.length() - 1);
    }

#ifdef PQ_USES_LITTLEFS
    if ( !PQ_FS.exists(m_prefix) ) PQ_FS.mkdir(m_prefix);
#endif
    m_initialized = true;
    if ( !isQueueEmpty(false) ) {
        // reset the internal counter to the latest number in case of restart after power failure
        findNextMessage(false, PQ_DEQUEUE_LATEST);
        m_counter = m_current_max+1;
// Serial.printf("PersistentQueue::begin - (%08x) m_counter set to %d (%d)\n", m_magic, m_counter, m_lastError);
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
}

 bool PersistentQueue::checkInitialized() {
    if ( !m_initialized) {
        m_lastError = PQ_ERROR_NOT_INITIALIZED;
        return false;
    }
    return true;
 }

/**
 * @brief Store message on the queue
 * 
 * @param name - 32 bit number to be used to name the queue file. 0 if internal counter to be used
 *               developer to supply increasing numbers - the messages will be sorted by the name.
 *               if the same number is already used, a subnumber 0-99 will be used. 
 *               messages with subnumbers are thought to have happened at the same time, therefore
 *               the concept of LATER/EARLIER does not apply
 *               messages are stored in:  /<prefix>/1234567890-99 format
 * @param data 
 * @param len 
 * @return true 
 * @return false 
 */
bool PersistentQueue::enqueue(uint32_t name, const uint8_t* data, size_t len) {
    if ( !checkInitialized()) return false;

    // if zero is provided as a name number - use internal counter
    if ( name == 0 ) name = m_counter++;

    char fn[PQ_MAX_FILENAME_SIZE+1];

    for (uint8_t i=0; i<PQ_MAX_SUBFILENAMES; i++) {
        //       0123456789012345678
        //      '/q/1234567890-00
        snprintf(fn, PQ_MAX_FILENAME_SIZE, "%s/%010d-%02d", m_prefix.c_str(), name, i);
// Serial.printf("PersistentQueue::enqueue - storing %s\n", fn);
        if ( !PQ_FS.exists(fn) ) {
            File F = PQ_FS.open(fn, "w+");
            if ( !F ) {
                m_lastError = PQ_ERROR_FILE_OP;
                return false;
            }
            //  write magic number first
            F.write((const uint8_t*) &m_magic, sizeof(uint32_t));
            
            //  write message
            F.write(data, len);

            //  write crc if requested
            if ( m_calcCRC) {
                uint32_t crc32 = pq_crc32(data, len);
                F.write((const uint8_t*) &crc32, sizeof(uint32_t));
            }

            F.close();
            m_lastError = PQ_ERROR_OK;
            return true;
        }
    }
    m_lastError = PQ_ERROR_OUT_OF_SUBNUMBERS;
    return false;
}


/**
 * @brief Check if queue is empty
 * 
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true - the queue is empty
 * @return false - queue has elements
 */
bool PersistentQueue::isQueueEmpty(bool fast_check) {
    if ( !checkInitialized()) return false;

    File root = PQ_FS.open(m_prefix);
  
    if(!root || !root.isDirectory()) {
        m_lastError = PQ_ERROR_INVALID_PREFIX;
        return false;
    }

    m_lastError = PQ_ERROR_OK;

    File file = root.openNextFile();
    while (file) {
        // for fast check we assume only messages of the same type (same magic number) are located in the queue folder
        if ( fast_check ) {
            file.close();
            return false;
        }
        else {
            uint32_t mn = 0;
            file.read((uint8_t*) &mn, sizeof(uint32_t));
            file.close();
            if ( mn == m_magic ) return false;
        }
        file = root.openNextFile();
    }
    return true;
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
    if ( !checkInitialized()) return false;

    if ( isQueueEmpty() ) {
        m_lastError = PQ_ERROR_QUEUE_EMPTY;
        return false;
    }

    String fn = findNextMessage(fast_check);
    String fp = fn;

    // IDF 4.x and above returns just name, not a path like 3.x:    
    if ( !fn.startsWith("/")) fp = m_prefix+'/'+fn;
// Serial.printf("PersistentQueue::dequeue - retrieving %s (%s)\n", fp.c_str(), fn.c_str());

    if ( fn.length() == 0 ) {
        m_lastError = PQ_ERROR_QUEUE_EMPTY; // ?
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
        m_lastError = PQ_ERROR_FILE_OP;
        return false;
    }

    fsz -= expected_min_len;

    // check magic number
    uint32_t mn = 0;
    F.read((uint8_t*) &mn, sizeof(uint32_t));
    if ( mn != m_magic ) {
        F.close();
        m_lastError = PQ_ERROR_INVALID_MAGIC;
        return false;
    }


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
    if ( F.read(data, fsz) != fsz ) {
        m_lastError = PQ_ERROR_FILE_OP;
        F.close();
        return false;
    }

    if ( m_calcCRC ) {
        uint32_t crc = 0;
        if ( F.read((uint8_t*) &crc, sizeof(uint32_t)) != sizeof(uint32_t) || crc != pq_crc32(data, fsz) ) {
            m_lastError = PQ_ERROR_BAD_CRC;
            F.close();
            return false;
        }
    }
    F.close();
    PQ_FS.remove(fp);

    m_lastError = PQ_ERROR_OK;
    return true;
}


/**
 * @brief Dequeue next message, allocate memory for the contents
 * 
 * @param data - pointer to a pointer - where the address of the buffer will be stored
 * @param len - actual length of the dequeued message
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true 
 * @return false 
 */
bool PersistentQueue::dequeue(uint8_t** data, size_t* len, bool fast_check) {
    if ( !checkInitialized()) return false;

    if ( isQueueEmpty() ) {
        m_lastError = PQ_ERROR_QUEUE_EMPTY;
        return false;
    }

    String fn = findNextMessage(fast_check);
    String fp = fn;

    // IDF 4.x and above returns just name, not a path like 3.x:
    if ( !fn.startsWith("/")) fp = m_prefix+'/'+fn;
// Serial.printf("PersistentQueue::dequeue - retrieving %s (%s)\n", fp.c_str(), fn.c_str());

    if ( fn.length() == 0 ) {
        m_lastError = PQ_ERROR_QUEUE_EMPTY; // ?
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
        m_lastError = PQ_ERROR_FILE_OP;
        return false;
    }

    fsz -= expected_min_len;

    // check magic number
    uint32_t mn = 0;
    F.read((uint8_t*) &mn, sizeof(uint32_t));
    if ( mn != m_magic ) {
        F.close();
        m_lastError = PQ_ERROR_INVALID_MAGIC;
        return false;
    }

    uint8_t* p = (uint8_t*) malloc(fsz);
    if ( p == NULL ) {
        m_lastError = PQ_ERROR_OUT_OF_MEMORY;
        F.close();
        return false;
    }

    memset(p, 0, fsz);
    if ( F.read(p, fsz) != fsz ) {
        m_lastError = PQ_ERROR_FILE_OP;
        F.close();
        free(p);
        p = NULL;
        return false;
    }

    if ( m_calcCRC ) {
        uint32_t crc = 0;
        if ( F.read((uint8_t*) &crc, sizeof(uint32_t)) != sizeof(uint32_t) || crc != pq_crc32(p, fsz) ) {
            m_lastError = PQ_ERROR_BAD_CRC;
            F.close();
            free(p);
            p = NULL;
            return false;
        }
    }
    F.close();
    PQ_FS.remove(fp);

    *data = p;
    *len = fsz;
    
    m_lastError = PQ_ERROR_OK;
    return true;
}


/**
 * @brief Find next message in the queue based on the desired ordering
 * 
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return String - sought filename of the queued message
 */
String PersistentQueue::findNextMessage(bool fast_check, pqDequeueOrder_t order) {
  if ( !checkInitialized()) return "";

  pqDequeueOrder_t ord = (order == PQ_DEQUEUE_DEFAULT) ? m_order : order;

  File root = PQ_FS.open(m_prefix);
  if (!root || !root.isDirectory()) {
    m_lastError = PQ_ERROR_INVALID_PREFIX;
    return "";
  }

  File file = root.openNextFile();

  uint32_t soughtNumber = (ord == PQ_DEQUEUE_OLDEST) ? UINT32_MAX : 1;
  String soughtFileName = "";
  
  while (file) {
    if (file.isDirectory()) {
      // Skip directories
      file = root.openNextFile();
      continue;
    }

    // if we are asked to check the message type - do it!
    if ( !fast_check ) {
        uint32_t mn = 0;
        file.read((uint8_t*) &mn, sizeof(uint32_t));
        if ( mn != m_magic ) {
            file.close();
            continue;
        }
    }

    String fileName = file.name();
    file.close(); // Close the current file before opening the next one

    //  Max number of digits in a 32 bit number is 10
    //       0123456789012345678
    //      '/q/1234567890-00
    //      '   1234567890  
    int prefixLen = m_prefix.length() + 1; // +1 for the '/'
    String fn = fileName.substring(prefixLen, prefixLen + 10);

    // Convert file name to a number
    uint32_t fileNumber = fn.toInt();

    // Check if the file name is a valid 32-bit number
    if (fileNumber == 0 && fn != "0") {
      file = root.openNextFile();
      continue;
    }

    // Update the lowest number and file name if necessary
    switch (ord) {
        case PQ_DEQUEUE_OLDEST:
            if (fileNumber < soughtNumber) {
                soughtNumber = fileNumber;
                soughtFileName = fileName;
            }
            break;

        case PQ_DEQUEUE_LATEST:
            if (fileNumber > soughtNumber) {
                soughtNumber = fileNumber;
                m_current_max = fileNumber;
                soughtFileName = fileName;
            }
            break;

        default:
            break;
    }

    file = root.openNextFile();
  }

  return soughtFileName;
}


/**
 * @brief Deletes all persistent messages from the queue folder
 * 
 * @param fast_check - assume all files are same correct type, do not check magic number
 * @return true - all messages deleted
 * @return false - some messages were not deleted
 */
bool PersistentQueue::purge(bool fast_check) {
  if ( !checkInitialized()) return false;

  bool result = true;

  File root = PQ_FS.open(m_prefix);
  if (!root || !root.isDirectory()) {
    m_lastError = PQ_ERROR_INVALID_PREFIX;
    return false;
  }

  File file = root.openNextFile();

    m_lastError = PQ_ERROR_OK;

  while (file) {
    if (file.isDirectory()) {
      // Skip directories
      file = root.openNextFile();
      continue;
    }

    // if we are asked to check the message type - do it!
    if ( !fast_check ) {
        uint32_t mn = 0;
        file.read((uint8_t*) &mn, sizeof(uint32_t));
        if ( mn != m_magic ) {
            file.close();
            continue;
        }
    }


    String fileName = file.name();
    // IDF 4.x and above returns just name, not a path like 3.x:
    if ( !fileName.startsWith("/") ) fileName = m_prefix + '/' + file.name();
    
    file.close();
    if ( !PQ_FS.remove(fileName) ) {
        result = false;
        m_lastError = PQ_ERROR_FILE_OP;
    }

    file = root.openNextFile();
  }

#ifdef PQ_USES_LITTLEFS
  PQ_FS.rmdir(m_prefix);
#endif

  return result;
}


// }