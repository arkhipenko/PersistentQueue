// Host model of the arduino-esp32 FS API used by PersistentQueue.
// Behavior follows core 3.3.9 (libraries/FS/src/FS.cpp, vfs_api.cpp, SPIFFS.cpp):
//  - File::name() returns the base name, File::path() the full path
//  - File::close() drops the handle, so operator bool is false afterwards
//  - flush() writes buffered data out; size() then reports what is on flash
//  - LittleFS model: real folders; open(path, "w") fails when the parent folder is missing
//  - SPIFFS model: flat; any path opens as a (virtual) folder; exists() is true for files only
//  - SPIFFS lists the files of nested "folders" too (ESP-IDF vfs_spiffs_readdir)
// Switches in namespace mockfs (reset() in the test restores the defaults):
//  - name_is_path:   emulate the IDF 3.x core, where name() returns the full path
//  - write_budget:   bytes that write() still accepts (-1: unlimited)
//  - flush_keep:     on flush(), keep only this many bytes of the file (-1: keep all);
//                    models buffered data lost when the file system is full
//  - fail_remove:    remove() fails
//  - fail_rename:    rename() fails
//  - skip_on_remove: a folder iteration skips one entry after each remove()
#pragma once
#include <Arduino.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace mockfs {
  extern bool name_is_path;
  extern long write_budget;
  extern long flush_keep;
  extern bool fail_remove;
  extern bool fail_rename;
  extern bool skip_on_remove;
}

struct MockStore {
  bool flat;                                              // true: SPIFFS model
  std::map<std::string, std::vector<uint8_t>> files;
  std::set<std::string> dirs;
  unsigned long removals = 0;
  explicit MockStore(bool f) : flat(f) { dirs.insert("/"); }
  static std::string parent(const std::string& p) {
    size_t i = p.find_last_of('/');
    return i == 0 ? "/" : p.substr(0, i);
  }
};

struct FileImpl {
  MockStore* st;
  std::string path;
  bool isDir = false;
  size_t pos = 0;
  std::vector<std::string> entries;   // folder snapshot
  size_t next = 0;
  unsigned long removals = 0;         // store removals seen by the iteration
};

class File {
  public:
    File() {}
    explicit File(std::shared_ptr<FileImpl> p) : p_(p) {}
    operator bool() const { return p_ != nullptr; }
    void close() { p_.reset(); }
    bool isDirectory() const { return p_ && p_->isDir; }
    const char* path() const { return p_ ? p_->path.c_str() : nullptr; }
    const char* name() const {
      if (!p_) return nullptr;
      if (mockfs::name_is_path) return p_->path.c_str();
      return p_->path.c_str() + p_->path.find_last_of('/') + 1;
    }
    size_t size() const {
      if (!p_ || p_->isDir) return 0;
      auto it = p_->st->files.find(p_->path);
      return it == p_->st->files.end() ? 0 : it->second.size();
    }
    size_t read(uint8_t* buf, size_t n) {
      if (!p_ || p_->isDir || !buf || !n) return 0;
      auto it = p_->st->files.find(p_->path);
      if (it == p_->st->files.end()) return 0;
      size_t k = 0;
      while (k < n && p_->pos < it->second.size()) buf[k++] = it->second[p_->pos++];
      return k;
    }
    size_t write(const uint8_t* buf, size_t n) {
      if (!p_ || p_->isDir || !buf || !n) return 0;
      auto& d = p_->st->files[p_->path];
      size_t k = 0;
      while (k < n && mockfs::write_budget != 0) {
        if (p_->pos < d.size()) d[p_->pos] = buf[k]; else d.push_back(buf[k]);
        p_->pos++; k++;
        if (mockfs::write_budget > 0) mockfs::write_budget--;
      }
      return k;
    }
    void flush() {
      if (!p_ || p_->isDir || mockfs::flush_keep < 0) return;
      auto& d = p_->st->files[p_->path];
      if (d.size() > (size_t) mockfs::flush_keep) d.resize((size_t) mockfs::flush_keep);
    }
    File openNextFile() {
      if (!p_ || !p_->isDir) return File();
      while (p_->next < p_->entries.size()) {
        if (mockfs::skip_on_remove && p_->st->removals != p_->removals) {
          p_->removals = p_->st->removals;
          p_->next++;                                     // the entry after a removed one is lost
          continue;
        }
        const std::string& e = p_->entries[p_->next++];
        if (!p_->st->files.count(e) && !p_->st->dirs.count(e)) continue;   // removed meanwhile
        auto f = std::make_shared<FileImpl>();
        f->st = p_->st;
        f->path = e;
        f->isDir = p_->st->dirs.count(e) > 0;
        return File(f);
      }
      return File();
    }
  private:
    std::shared_ptr<FileImpl> p_;
};

class FSMock {
  public:
    explicit FSMock(bool flat) : st_(flat) {}
    MockStore& store() { return st_; }
    bool begin(bool = false) { return true; }
    File open(const String& path, const char* mode = "r") { return open(path.c_str(), mode); }
    File open(const char* path, const char* mode = "r") {
      std::string p(path);
      if (p.empty() || p[0] != '/') return File();
      auto f = std::make_shared<FileImpl>();
      f->st = &st_;
      f->path = p;
      if (mode[0] == 'r') {
        if (st_.files.count(p)) return File(f);
        if (st_.flat || st_.dirs.count(p)) {
          f->isDir = true;
          f->removals = st_.removals;
          std::string pre = (p == "/") ? "/" : p + "/";
          for (auto& kv : st_.files)
            if (kv.first.compare(0, pre.size(), pre) == 0 &&
                (st_.flat || kv.first.find('/', pre.size()) == std::string::npos))
              f->entries.push_back(kv.first);
          if (!st_.flat)
            for (auto& d : st_.dirs)
              if (d != p && d.compare(0, pre.size(), pre) == 0 && d.find('/', pre.size()) == std::string::npos)
                f->entries.push_back(d);
          std::sort(f->entries.begin(), f->entries.end());
          return File(f);
        }
        return File();
      }
      if (!st_.flat && !st_.dirs.count(MockStore::parent(p))) return File();
      st_.files[p].clear();
      return File(f);
    }
    bool exists(const char* path) {
      std::string p(path);
      return st_.files.count(p) || (!st_.flat && st_.dirs.count(p));
    }
    bool exists(const String& p) { return exists(p.c_str()); }
    bool remove(const String& p) {
      if (mockfs::fail_remove) return false;
      if (!st_.files.erase(p.str())) return false;
      st_.removals++;
      return true;
    }
    bool rename(const String& from, const String& to) {
      if (mockfs::fail_rename || !st_.files.count(from.str())) return false;
      if (st_.flat && st_.files.count(to.str())) return false;   // SPIFFS: target must not exist
      if (!st_.flat && !st_.dirs.count(MockStore::parent(to.str()))) return false;
      st_.files[to.str()] = st_.files[from.str()];
      st_.files.erase(from.str());
      return true;
    }
    bool mkdir(const String& p) { if (st_.flat) return false; st_.dirs.insert(p.str()); return true; }
    bool rmdir(const String& p) {
      if (st_.flat) return false;
      std::string pre = p.str() + "/";
      for (auto& kv : st_.files) if (kv.first.compare(0, pre.size(), pre) == 0) return false;
      return st_.dirs.erase(p.str()) > 0;
    }
  private:
    MockStore st_;
};
