// Host tests for PersistentQueue. Built once with -DPQ_USES_SPIFFS and once with
// -DPQ_USES_LITTLEFS against the FS model in stubs/FS.h (see the Makefile).
#include <PersistentQueue.h>
#include <new>

FSMock SPIFFS(true);
FSMock LittleFS(false);

namespace mockfs {
  bool name_is_path = false;
  long write_budget = -1;
  long flush_keep = -1;
  bool fail_remove = false;
  bool fail_rename = false;
  bool skip_on_remove = false;
}

static int checks = 0;
static int failures = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool isLittleFS() { return !PQ_FS.store().flat; }

#ifdef PQ_TEST_IDF3
static const bool kIdf3 = true;     // arduino-esp32 1.x: File::name() is the full path
#else
static const bool kIdf3 = false;
#endif

static void reset() {
  PQ_FS.store().files.clear();
  PQ_FS.store().dirs.clear();
  PQ_FS.store().dirs.insert("/");
  mockfs::name_is_path = kIdf3;
  mockfs::write_budget = -1;
  mockfs::flush_keep = -1;
  mockfs::fail_remove = false;
  mockfs::fail_rename = false;
  mockfs::skip_on_remove = false;
}

static size_t files() { return PQ_FS.store().files.size(); }
static bool hasFile(const char* p) { return PQ_FS.store().files.count(p) > 0; }
static std::vector<uint8_t>& content(const char* p) { return PQ_FS.store().files[p]; }

static bool put(PersistentQueue& q, uint32_t name, const char* s) {
  return q.enqueue(name, (const uint8_t*) s, strlen(s) + 1);
}

// dequeue (or peek) with the allocating overload; "<err N>" on failure
static std::string take(PersistentQueue& q, bool fast = true, bool peek = false) {
  uint8_t* p = nullptr;
  size_t l = 0;
  bool ok = peek ? q.peek(&p, &l, fast) : q.dequeue(&p, &l, fast);
  if (!ok) return "<err " + std::to_string(q.getLastError()) + ">";
  std::string r((const char*) p);
  free(p);
  return r;
}

// n dequeues concatenated, in call order
static std::string takeN(PersistentQueue& q, int n) {
  std::string r;
  for (int i = 0; i < n; i++) r += take(q);
  return r;
}

static std::string err(pqError_t e) { return "<err " + std::to_string(e) + ">"; }

// writes a raw message file as 1.x would (magic, data, optional crc)
static void rawFile(const char* path, uint32_t magic, const char* s, bool crc = true) {
  File f = PQ_FS.open(path, "w");
  f.write((const uint8_t*) &magic, 4);
  f.write((const uint8_t*) s, strlen(s) + 1);
  if (crc) {
    uint32_t c = 0xffffffff;
    static const uint32_t tab[16] = {
      0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
      0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c };
    for (size_t i = 0; i <= strlen(s); i++) {
      c ^= (uint8_t) s[i];
      c = tab[c & 0x0f] ^ (c >> 4);
      c = tab[c & 0x0f] ^ (c >> 4);
    }
    f.write((const uint8_t*) &c, 4);
  }
  f.close();
}

static void test_basic() {
  printf("basic FIFO, count, isQueueEmpty\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1"));
  CHECK(q.isQueueEmpty() && q.isQueueEmpty(false));
  CHECK(q.count() == 0);
  CHECK(put(q, 0, "a") && put(q, 0, "b") && put(q, 0, "c"));
  CHECK(hasFile("/fq1/0000000001-00") && hasFile("/fq1/0000000003-00"));
  CHECK(q.count() == 3 && q.count(false) == 3);
  CHECK(!q.isQueueEmpty() && !q.isQueueEmpty(false));
  CHECK(take(q) == "a" && take(q) == "b" && take(q, false) == "c");
  CHECK(q.count() == 0 && q.isQueueEmpty());
  CHECK(take(q) == err(PQ_ERROR_QUEUE_EMPTY));
  CHECK(files() == 0);
}

static void test_not_initialized() {
  printf("calls before begin() and after end()\n"); reset();
  PersistentQueue q;
  uint8_t* p = nullptr; size_t l = 0; uint8_t buf[8];
  CHECK(!put(q, 0, "a") && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(q.count() == 0 && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(!q.isQueueEmpty() && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(!q.dequeue(&p, &l) && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(!q.dequeue(buf, sizeof buf, &l) && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(!q.peek(&p, &l) && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(!q.drop() && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(!q.purge() && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(q.begin("/fq1") && put(q, 0, "a"));
  q.end();
  CHECK(q.getLastError() == PQ_ERROR_OK);
  CHECK(q.count() == 0 && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);
  CHECK(q.begin("/fq1") && q.count() == 1);
}

static void test_prefix() {
  printf("D11: prefix normalization and validation\n"); reset();
  PersistentQueue q;
  CHECK(!q.begin(nullptr) && q.getLastError() == PQ_ERROR_INVALID_PREFIX);
  CHECK(!q.begin("") && q.getLastError() == PQ_ERROR_INVALID_PREFIX);
  CHECK(!q.begin("/") && q.getLastError() == PQ_ERROR_INVALID_PREFIX);
  CHECK(!q.begin("///") && q.getLastError() == PQ_ERROR_INVALID_PREFIX);
  CHECK(!put(q, 0, "a") && q.getLastError() == PQ_ERROR_NOT_INITIALIZED);

  std::string longest = "/" + std::string(PQ_MAX_PREFIX_SIZE - 1, 'p');
  std::string tooLong = longest + "p";
  CHECK(!q.begin(tooLong.c_str()) && q.getLastError() == PQ_ERROR_INVALID_PREFIX);
  CHECK(q.begin((longest + "//").c_str()));
  CHECK(put(q, 0, "a") && q.count() == 1 && take(q) == "a");
  for (auto& kv : PQ_FS.store().files) CHECK(kv.first.size() <= (size_t) PQ_MAX_FILENAME_SIZE);

  CHECK(q.begin("fq2/") && put(q, 7, "x"));
  CHECK(hasFile("/fq2/0000000007-00"));
  if (isLittleFS()) CHECK(PQ_FS.store().dirs.count("/fq2") == 1);
}

static void test_order_prefix_lengths() {
  printf("D1: order for every prefix length, both orders, both core name modes\n");
  for (int idf3 = 0; idf3 <= 1; idf3++) {
    for (int len = 2; len <= PQ_MAX_PREFIX_SIZE; len++) {
      reset();
      mockfs::name_is_path = idf3 || kIdf3;
      std::string prefix = "/" + std::string(len - 1, 'q');
      PersistentQueue qo(PQ_DEFAULT_MN, PQ_DEQUEUE_OLDEST), ql(0x12345678, PQ_DEQUEUE_LATEST);
      CHECK(qo.begin(prefix.c_str()));
      CHECK(put(qo, 150000, "B") && put(qo, 99999, "A") && put(qo, 4000000000u, "D") && put(qo, 1234567890, "C"));
      std::string got = takeN(qo, 4);
      CHECK(got == "ABCD");
      if (got != "ABCD") printf("    prefix length %d idf3 %d: OLDEST gave %s\n", len, idf3, got.c_str());

      CHECK(ql.begin(prefix.c_str()));
      CHECK(put(ql, 150000, "B") && put(ql, 99999, "A") && put(ql, 4000000000u, "D") && put(ql, 1234567890, "C"));
      got = takeN(ql, 4);
      CHECK(got == "DCBA");
      if (got != "DCBA") printf("    prefix length %d idf3 %d: LATEST gave %s\n", len, idf3, got.c_str());
    }
  }
}

static void test_dti_epoch() {
  printf("D1: dti usage, prefix /q, epoch seconds across 1800000000\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/q"));
  CHECK(put(q, 1800000005u, "after") && put(q, 1799999990u, "before"));
  CHECK(take(q) == "before" && take(q) == "after");
}

static void test_latest_one() {
  printf("D2: LATEST returns message 1\n"); reset();
  PersistentQueue q(PQ_DEFAULT_MN, PQ_DEQUEUE_LATEST);
  CHECK(q.begin("/fq2"));
  CHECK(put(q, 0, "m1") && put(q, 0, "m2") && put(q, 0, "m3"));
  CHECK(take(q) == "m3" && take(q) == "m2" && take(q) == "m1");
  CHECK(q.count() == 0);
}

static void test_subnumbers() {
  printf("same name: subnumbers, order by subnumber, OUT_OF_SUBNUMBERS\n"); reset();
  PersistentQueue q, ql(PQ_DEFAULT_MN, PQ_DEQUEUE_LATEST);
  CHECK(q.begin("/s"));
  CHECK(put(q, 5, "x0") && put(q, 5, "x1") && put(q, 5, "x2"));
  CHECK(hasFile("/s/0000000005-02"));
  CHECK(take(q) == "x0" && take(q) == "x1" && take(q) == "x2");
  CHECK(ql.begin("/s"));
  CHECK(put(ql, 5, "y0") && put(ql, 5, "y1"));
  CHECK(take(ql) == "y1" && take(ql) == "y0");
  for (int i = 0; i < PQ_MAX_SUBFILENAMES; i++) CHECK(put(q, 9, "z"));
  CHECK(!put(q, 9, "z") && q.getLastError() == PQ_ERROR_OUT_OF_SUBNUMBERS);
  CHECK(q.count() == PQ_MAX_SUBFILENAMES);
}

static void test_foreign_magic_full_check() {
  printf("D3: full check skips other message types and continues the scan\n"); reset();
  PersistentQueue q, other(0x11111111);
  CHECK(q.begin("/fq1") && other.begin("/fq1"));
  CHECK(put(other, 1, "x") && put(q, 2, "a") && put(other, 3, "y") && put(q, 4, "b"));
  CHECK(q.count() == 4 && q.count(false) == 2 && other.count(false) == 2);
  CHECK(take(q, false) == "a");
  CHECK(q.purge(false));
  CHECK(q.count(false) == 0 && other.count(false) == 2);
  CHECK(take(other, false) == "x" && take(other, false) == "y");
}

static void test_bad_head() {
  printf("D4: a bad message is removed and the queue moves on\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1"));
  CHECK(put(q, 0, "a") && put(q, 0, "b") && put(q, 0, "c") && put(q, 0, "d"));
  content("/fq1/0000000001-00")[4] ^= 0xFF;                   // data byte: bad crc
  content("/fq1/0000000002-00").resize(3);                    // shorter than the magic number
  content("/fq1/0000000003-00")[0] ^= 0xFF;                   // magic number
  CHECK(take(q) == err(PQ_ERROR_BAD_CRC));
  CHECK(take(q) == err(PQ_ERROR_FILE_OP));
  CHECK(take(q) == err(PQ_ERROR_INVALID_MAGIC));
  CHECK(take(q) == "d");
  CHECK(files() == 0);

  printf("D4: bad message met by peek() and by the buffer dequeue()\n"); reset();
  PersistentQueue r;
  CHECK(r.begin("/fq1") && put(r, 0, "a") && put(r, 0, "b"));
  content("/fq1/0000000001-00")[5] ^= 0xFF;
  CHECK(take(r, true, true) == err(PQ_ERROR_BAD_CRC));
  CHECK(take(r, true, true) == "b" && r.count() == 1);
  CHECK(put(r, 0, "c"));
  content("/fq1/0000000003-00").resize(9);                    // one byte short
  char buf[32]; size_t l = 0;
  CHECK(r.dequeue((uint8_t*) buf, sizeof buf, &l));           // "b" first
  CHECK(!r.dequeue((uint8_t*) buf, sizeof buf, &l) && r.getLastError() == PQ_ERROR_BAD_CRC);
  CHECK(r.count() == 0);
}

static void test_write_failure() {
  printf("D5: write failures are reported and leave no files\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1"));
  for (long budget = 0; budget < 4 + 12 + 4; budget++) {
    mockfs::write_budget = budget;
    CHECK(!put(q, 0, "hello world") && q.getLastError() == PQ_ERROR_FILE_OP);
    CHECK(files() == 0);
  }
  mockfs::write_budget = -1;
  mockfs::flush_keep = 10;
  CHECK(!put(q, 0, "hello world") && q.getLastError() == PQ_ERROR_FILE_OP);
  CHECK(files() == 0);
  mockfs::flush_keep = -1;
  mockfs::fail_rename = true;
  CHECK(!put(q, 0, "hello world") && q.getLastError() == PQ_ERROR_FILE_OP);
  CHECK(files() == 0);
  mockfs::fail_rename = false;
  CHECK(put(q, 0, "hello world") && take(q) == "hello world");
}

static void test_temp_files() {
  printf("D5: temp files of an interrupted enqueue\n"); reset();
  if (isLittleFS()) PQ_FS.mkdir("/fq1");
  rawFile("/fq1/~0000000007-00", PQ_DEFAULT_MN, "partial");
  rawFile("/fq1/0000000002-00", PQ_DEFAULT_MN, "whole");
  PersistentQueue q;
  CHECK(q.begin("/fq1"));
  CHECK(!hasFile("/fq1/~0000000007-00"));
  CHECK(q.count() == 1 && q.count(false) == 1);
  CHECK(put(q, 0, "next"));
  CHECK(hasFile("/fq1/0000000003-00"));                       // counter continues from 2, not from the temp file
  rawFile("/fq1/~0000000009-00", PQ_DEFAULT_MN, "partial");   // appears while running
  CHECK(q.count() == 2 && !q.isQueueEmpty());
  CHECK(take(q) == "whole" && take(q) == "next" && q.isQueueEmpty());
  CHECK(q.purge() && files() == 0);                            // purge removes temp files too
}

static void test_counter_restore() {
  printf("D6: counter restore after a restart\n"); reset();
  { PersistentQueue q; CHECK(q.begin("/fq1") && put(q, 0, "a")); }
  alignas(PersistentQueue) static unsigned char mem[sizeof(PersistentQueue)];
  memset(mem, 0xAB, sizeof mem);
  PersistentQueue* q = new (mem) PersistentQueue();
  CHECK(q->begin("/fq1") && put(*q, 0, "b"));
  CHECK(hasFile("/fq1/0000000002-00"));
  q->~PersistentQueue();

  PersistentQueue r;
  CHECK(r.begin("/fq1") && put(r, 0, "c") && hasFile("/fq1/0000000003-00"));
  CHECK(take(r) == "a" && take(r) == "b" && take(r) == "c");
}

static void test_high_names() {
  printf("D7: names from 2^31 up, and 1.x negative file names\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1"));
  CHECK(put(q, 3000000000u, "new"));
  CHECK(hasFile("/fq1/3000000000-00"));
  CHECK(put(q, UINT32_MAX, "max") && hasFile("/fq1/4294967295-00"));
  rawFile("/fq1/-1294967296-01", PQ_DEFAULT_MN, "old");        // 3000000000 as written by 1.1.0
  rawFile("/fq1/-000000002-00", PQ_DEFAULT_MN, "old max-1");   // 4294967294 as written by 1.1.0
  CHECK(put(q, 2999999999u, "first"));
  CHECK(q.count() == 5);
  CHECK(take(q) == "first" && take(q) == "new" && take(q) == "old");
  CHECK(take(q) == "old max-1" && take(q) == "max");

  printf("    counter restore from the highest name\n"); reset();
  PersistentQueue r;
  CHECK(r.begin("/fq1") && put(r, UINT32_MAX, "top"));
  PersistentQueue s;
  CHECK(s.begin("/fq1") && put(s, 0, "wrapped") && hasFile("/fq1/0000000001-00"));
}

static void test_default_order() {
  printf("D8: PQ_DEQUEUE_DEFAULT in the constructor works as OLDEST\n"); reset();
  PersistentQueue q(PQ_DEFAULT_MN, PQ_DEQUEUE_DEFAULT);
  CHECK(q.begin("/fq1") && put(q, 2, "b") && put(q, 1, "a"));
  CHECK(take(q) == "a" && take(q) == "b");
}

static void test_purge() {
  printf("D9: purge keeps the folder and the queue usable\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1") && put(q, 0, "a") && put(q, 0, "b"));
  CHECK(q.purge() && q.count() == 0 && files() == 0);
  if (isLittleFS()) CHECK(PQ_FS.store().dirs.count("/fq1") == 1);
  CHECK(put(q, 0, "c") && take(q) == "c");

  printf("    purge with folder iteration that skips entries after a remove\n"); reset();
  CHECK(q.begin("/fq1"));
  for (int i = 0; i < 20; i++) CHECK(put(q, 0, "m"));
  mockfs::skip_on_remove = true;
  CHECK(q.purge() && q.count() == 0 && files() == 0);

  printf("    purge reports a failed remove\n"); reset();
  CHECK(q.begin("/fq1") && put(q, 0, "a"));
  mockfs::fail_remove = true;
  CHECK(!q.purge() && q.getLastError() == PQ_ERROR_FILE_OP);
  mockfs::fail_remove = false;
  CHECK(q.count() == 1);
}

static void test_other_entries() {
  printf("D10: sub-folders and other files are not messages\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1"));
  if (isLittleFS()) PQ_FS.mkdir("/fq1/sub");
  const char* others[] = { "/fq1/notes.txt", "/fq1/123456789-00", "/fq1/12345678901-00",
                           "/fq1/0000000001-0", "/fq1/0000000001_00", "/fq1/00000000x1-00", "/fq1/-0000000000-00" };
  for (auto o : others) rawFile(o, PQ_DEFAULT_MN, "not a message");
  CHECK(q.count() == 0 && q.count(false) == 0);
  CHECK(q.isQueueEmpty() && q.isQueueEmpty(false));
  CHECK(take(q) == err(PQ_ERROR_QUEUE_EMPTY));
  CHECK(!q.drop() && q.getLastError() == PQ_ERROR_QUEUE_EMPTY);
  CHECK(put(q, 0, "a") && q.count() == 1 && take(q) == "a");
  CHECK(q.purge() && files() == sizeof(others) / sizeof(others[0]));
}

static void test_nested_prefix() {
  printf("nested queue folders: /q and /q/a\n"); reset();
  PersistentQueue outer, inner;
  CHECK(outer.begin("/q") && inner.begin("/q/a"));
  CHECK(put(inner, 1, "inner1") && put(outer, 2, "outer2") && put(inner, 3, "inner3"));
  CHECK(outer.count() == 1 && inner.count() == 2);
  CHECK(take(outer) == "outer2" && take(outer) == err(PQ_ERROR_QUEUE_EMPTY));
  CHECK(outer.purge() && inner.count() == 2);
  CHECK(take(inner) == "inner1" && take(inner) == "inner3");
}

static void test_peek_drop() {
  printf("peek() and drop()\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1") && put(q, 0, "a") && put(q, 0, "b"));
  CHECK(take(q, true, true) == "a" && q.count() == 2);
  CHECK(take(q, true, true) == "a");                          // peek again: same message
  CHECK(q.drop() && q.count() == 1);
  char buf[8]; size_t l = 0;
  CHECK(q.peek((uint8_t*) buf, sizeof buf, &l) && std::string(buf) == "b" && l == 2);
  CHECK(q.drop() && q.count() == 0);
  CHECK(!q.drop() && q.getLastError() == PQ_ERROR_QUEUE_EMPTY);

  printf("    drop() without peek() removes the next message\n");
  CHECK(put(q, 0, "c") && put(q, 0, "d"));
  CHECK(q.drop() && take(q) == "d");

  printf("    dequeue() cancels a pending peek\n");
  CHECK(put(q, 0, "e") && put(q, 0, "f"));
  CHECK(take(q, true, true) == "e" && take(q) == "e");
  CHECK(q.drop() && q.count() == 0);                          // removed the next message: f

  printf("    LATEST: a message stored between peek() and drop() survives\n"); reset();
  PersistentQueue ql(PQ_DEFAULT_MN, PQ_DEQUEUE_LATEST);
  CHECK(ql.begin("/fq2") && put(ql, 10, "old"));
  CHECK(take(ql, true, true) == "old");
  CHECK(put(ql, 20, "new") && ql.drop());
  CHECK(ql.count() == 1 && take(ql) == "new");

  printf("    drop() of a peeked message that is gone\n"); reset();
  CHECK(q.begin("/fq1") && put(q, 0, "a"));
  CHECK(take(q, true, true) == "a");
  PQ_FS.store().files.clear();
  CHECK(!q.drop() && q.getLastError() == PQ_ERROR_FILE_OP);
}

static void test_buffers() {
  printf("buffer overloads, NULL arguments, empty messages\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1") && put(q, 0, "hello"));
  char buf[16]; size_t l = 0; uint8_t* p = nullptr;
  CHECK(!q.dequeue((uint8_t*) buf, 3, &l) && q.getLastError() == PQ_ERROR_SMALL_BUFFER && l == 6);
  l = 0;
  CHECK(!q.dequeue(nullptr, 0, &l) && q.getLastError() == PQ_ERROR_SMALL_BUFFER && l == 6);   // size query
  CHECK(!q.dequeue((uint8_t*) buf, sizeof buf, nullptr) && q.getLastError() == PQ_ERROR_NULL_POINTER);
  CHECK(!q.dequeue(nullptr, &l) && q.getLastError() == PQ_ERROR_NULL_POINTER);
  CHECK(!q.dequeue(&p, nullptr) && q.getLastError() == PQ_ERROR_NULL_POINTER);
  CHECK(!q.dequeue(nullptr, sizeof buf, &l) && q.getLastError() == PQ_ERROR_NULL_POINTER);
  CHECK(q.count() == 1);
  memset(buf, 'x', sizeof buf);
  CHECK(q.dequeue((uint8_t*) buf, sizeof buf, &l) && l == 6 && std::string(buf) == "hello" && buf[15] == 0);

  CHECK(!q.enqueue(0, nullptr, 3) && q.getLastError() == PQ_ERROR_NULL_POINTER);
  CHECK(q.enqueue(0, nullptr, 0) && q.count() == 1);
  l = 99;
  CHECK(q.dequeue(&p, &l) && l == 0 && p != nullptr);
  free(p);
  CHECK(q.enqueue(0, nullptr, 0));
  l = 99;
  CHECK(q.dequeue((uint8_t*) buf, sizeof buf, &l) && l == 0);
}

static void test_remove_failure() {
  printf("dequeue() when the file cannot be removed\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1") && put(q, 0, "a"));
  mockfs::fail_remove = true;
  CHECK(take(q) == err(PQ_ERROR_FILE_OP) && q.count() == 1);
  mockfs::fail_remove = false;
  CHECK(take(q) == "a" && q.count() == 0);
}

static void test_limit() {
  printf("setMaxMessages(): reject when full\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/fq1") && q.getMaxMessages() == 0);
  q.setMaxMessages(2);
  CHECK(q.getMaxMessages() == 2);
  CHECK(put(q, 0, "a") && put(q, 0, "b"));
  CHECK(!put(q, 0, "c") && q.getLastError() == PQ_ERROR_QUEUE_FULL && q.count() == 2);
  CHECK(take(q) == "a" && put(q, 0, "c") && q.count() == 2);

  printf("    drop the oldest when full\n"); reset();
  PersistentQueue ql(PQ_DEFAULT_MN, PQ_DEQUEUE_LATEST);
  CHECK(ql.begin("/fq1"));
  ql.setMaxMessages(3, true);
  for (int i = 1; i <= 5; i++) CHECK(ql.enqueue(0, (const uint8_t*) "m", 2));
  CHECK(ql.count() == 3 && hasFile("/fq1/0000000003-00") && !hasFile("/fq1/0000000002-00"));

  printf("    lowering the limit removes several\n");
  ql.setMaxMessages(1, true);
  CHECK(put(ql, 0, "last") && ql.count() == 1 && take(ql) == "last");

  printf("    the limit counts this queue's messages only\n"); reset();
  PersistentQueue a, b(0x22222222);
  CHECK(a.begin("/fq1") && b.begin("/fq1"));
  a.setMaxMessages(1, true);
  CHECK(put(b, 1, "b1") && put(b, 2, "b2"));
  CHECK(put(a, 3, "a1") && put(a, 4, "a2"));
  CHECK(a.count(false) == 1 && b.count(false) == 2);

  printf("    oldest dropped while it was peeked\n"); reset();
  PersistentQueue c;
  CHECK(c.begin("/fq1") && put(c, 0, "x"));
  c.setMaxMessages(1, true);
  CHECK(take(c, true, true) == "x");
  CHECK(put(c, 0, "y"));
  CHECK(!c.drop() && c.getLastError() == PQ_ERROR_FILE_OP);   // the peeked message is gone; y is not touched
  CHECK(c.count() == 1 && take(c) == "y");
}

static void test_crc_off_and_format() {
  printf("file format: worked example, CRC off\n"); reset();
  PersistentQueue q;
  CHECK(q.begin("/q") && q.enqueue(1790000000u, (const uint8_t*) "hi", 3));
  const uint8_t expect[] = { 0xDE, 0xC0, 0x5A, 0xA5, 0x68, 0x69, 0x00, 0x9B, 0x6C, 0x45, 0xF2 };
  CHECK(content("/q/1790000000-00") == std::vector<uint8_t>(expect, expect + sizeof expect));

  PersistentQueue n(0x01020304, PQ_DEQUEUE_OLDEST, false);
  CHECK(n.begin("/n") && put(n, 0, "abc"));
  CHECK(content("/n/0000000001-00").size() == 4 + 4);
  CHECK(content("/n/0000000001-00")[0] == 0x04);
  CHECK(take(n) == "abc");
}

static void test_manifests() {
  printf("version: header, library.json, library.properties, README\n");
  auto slurp = [](const char* path) {
    std::string s;
    FILE* f = fopen(path, "rb");
    if (!f) return s;
    char b[512]; size_t k;
    while ((k = fread(b, 1, sizeof b, f)) > 0) s.append(b, k);
    fclose(f);
    return s;
  };
  std::string json = slurp(PQ_REPO_DIR "/library.json");
  std::string props = slurp(PQ_REPO_DIR "/library.properties");
  CHECK(json.find("\"version\": \"" PQ_VERSION_STRING "\"") != std::string::npos);
  CHECK(props.find("version=" PQ_VERSION_STRING) != std::string::npos);
  std::string readme = slurp(PQ_REPO_DIR "/README.md");
  CHECK(readme.find("#### Version " PQ_VERSION_STRING ":") != std::string::npos);
  CHECK(readme.find("- " PQ_VERSION_STRING " (") != std::string::npos);    // version history entry
  int major = 0, minor = 0, patch = 0;
  CHECK(sscanf(PQ_VERSION_STRING, "%d.%d.%d", &major, &minor, &patch) == 3);
  CHECK(PQ_VERSION == major * 10000 + minor * 100 + patch);
}

int main() {
  printf("=== PersistentQueue host tests: %s model%s (max path %d, max prefix %d)\n",
         PQ_FS.store().flat ? "SPIFFS" : "LittleFS", kIdf3 ? ", IDF 3.x core" : "", PQ_MAX_FILENAME_SIZE, PQ_MAX_PREFIX_SIZE);
  test_basic();
  test_not_initialized();
  test_prefix();
  test_order_prefix_lengths();
  test_dti_epoch();
  test_latest_one();
  test_subnumbers();
  test_foreign_magic_full_check();
  test_bad_head();
  test_write_failure();
  test_temp_files();
  test_counter_restore();
  test_high_names();
  test_default_order();
  test_purge();
  test_other_entries();
  test_nested_prefix();
  test_peek_drop();
  test_buffers();
  test_remove_failure();
  test_limit();
  test_crc_off_and_format();
  test_manifests();
  printf("=== %d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
