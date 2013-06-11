// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ---
// Author: Sainbayar Sukhbaatar
//         Dai Mikurube
//
// This file contains a class DeepHeapProfile and its public function
// DeepHeapProfile::DumpOrderedProfile().  The function works like
// HeapProfileTable::FillOrderedProfile(), but dumps directory to files.
//
// DeepHeapProfile::DumpOrderedProfile() dumps more detailed information about
// heap usage, which includes OS-level information such as memory residency and
// type information if the type profiler is available.
//
// DeepHeapProfile::DumpOrderedProfile() uses data stored in HeapProfileTable.
// Any code in DeepHeapProfile runs only when DumpOrderedProfile() is called.
// It has overhead in dumping, but no overhead in logging.
//
// It currently works only on Linux including Android.  It does nothing in
// non-Linux environments.

// Note that uint64 is used to represent addresses instead of uintptr_t, and
// int is used to represent buffer sizes instead of size_t.
// It's for consistency with other TCMalloc functions.  ProcMapsIterator uses
// uint64 for addresses, and HeapProfileTable::DumpOrderedProfile uses int
// for buffer sizes.

#ifndef BASE_DEEP_HEAP_PROFILE_H_
#define BASE_DEEP_HEAP_PROFILE_H_

#include "config.h"

#if defined(TYPE_PROFILING)
#include <typeinfo>
#endif

#if defined(__linux__)
#define USE_DEEP_HEAP_PROFILE 1
#endif

#include "addressmap-inl.h"
#include "heap-profile-table.h"
#include "memory_region_map.h"

class DeepHeapProfile {
 public:
  // Defines an interface for getting info about memory residence.
  class MemoryResidenceInfoGetterInterface {
   public:
    virtual ~MemoryResidenceInfoGetterInterface();

    // Initializes the instance.
    virtual void Initialize() = 0;

    // Returns the number of resident (including swapped) bytes of the given
    // memory region from |first_address| to |last_address| inclusive.
    virtual size_t CommittedSize(
        uint64 first_address, uint64 last_address) const = 0;

    // Creates a new platform specific MemoryResidenceInfoGetterInterface.
    static MemoryResidenceInfoGetterInterface* Create();

   protected:
    MemoryResidenceInfoGetterInterface();
  };

  // Constructs a DeepHeapProfile instance.  It works as a wrapper of
  // HeapProfileTable.
  //
  // |heap_profile| is a pointer to HeapProfileTable.  DeepHeapProfile reads
  // data in |heap_profile| and forwards operations to |heap_profile| if
  // DeepHeapProfile is not available (non-Linux).
  // |prefix| is a prefix of dumped file names.
  DeepHeapProfile(HeapProfileTable* heap_profile, const char* prefix);
  ~DeepHeapProfile();

  // Dumps a deep profile into |fd| with using |raw_buffer| of |buffer_size|.
  //
  // In addition, a list of buckets is dumped into a ".buckets" file in
  // descending order of allocated bytes.
  void DumpOrderedProfile(const char* reason,
                          char raw_buffer[],
                          int buffer_size,
                          RawFD fd);

 private:
#ifdef USE_DEEP_HEAP_PROFILE
  typedef HeapProfileTable::Stats Stats;
  typedef HeapProfileTable::Bucket Bucket;
  typedef HeapProfileTable::AllocValue AllocValue;
  typedef HeapProfileTable::AllocationMap AllocationMap;

  enum MapsRegionType {
    // Bytes of memory which were not recognized with /proc/<pid>/maps.
    // This size should be 0.
    ABSENT,

    // Bytes of memory which is mapped anonymously.
    // Regions which contain nothing in the last column of /proc/<pid>/maps.
    ANONYMOUS,

    // Bytes of memory which is mapped to a executable/non-executable file.
    // Regions which contain file paths in the last column of /proc/<pid>/maps.
    FILE_EXEC,
    FILE_NONEXEC,

    // Bytes of memory which is labeled [stack] in /proc/<pid>/maps.
    STACK,

    // Bytes of memory which is labeled, but not mapped to any file.
    // Regions which contain non-path strings in the last column of
    // /proc/<pid>/maps.
    OTHER,

    NUMBER_OF_MAPS_REGION_TYPES
  };

  static const char* kMapsRegionTypeDict[NUMBER_OF_MAPS_REGION_TYPES];

  // Manages a buffer to keep a text to be dumped to a file.
  class TextBuffer {
   public:
    TextBuffer(char *raw_buffer, int size, RawFD fd)
        : buffer_(raw_buffer),
          size_(size),
          cursor_(0),
          fd_(fd) {
    }

    int Size();
    int FilledBytes();
    void Clear();
    void Flush();

    bool AppendChar(char value);
    bool AppendString(const char* value, int width);
    bool AppendInt(int value, int width, bool leading_zero);
    bool AppendLong(long value, int width);
    bool AppendUnsignedLong(unsigned long value, int width);
    bool AppendInt64(int64 value, int width);
    bool AppendPtr(uint64 value, int width);

   private:
    bool ForwardCursor(int appended);

    char *buffer_;
    int size_;
    int cursor_;
    RawFD fd_;
    DISALLOW_COPY_AND_ASSIGN(TextBuffer);
  };

  // Contains extended information for HeapProfileTable::Bucket.  These objects
  // are managed in a hash table (DeepBucketTable) whose key is an address of
  // a Bucket and other additional information.
  struct DeepBucket {
   public:
    void UnparseForStats(TextBuffer* buffer);
    void UnparseForBucketFile(TextBuffer* buffer);

    Bucket* bucket;
#if defined(TYPE_PROFILING)
    const std::type_info* type;  // A type of the object
#endif
    size_t committed_size;  // A resident size of this bucket
    bool is_mmap;  // True if the bucket represents a mmap region
    int id;  // A unique ID of the bucket
    bool is_logged;  // True if the stracktrace is logged to a file
    DeepBucket* next;  // A reference to the next entry in the hash table
  };

  // Manages a hash table for DeepBucket.
  class DeepBucketTable {
   public:
    DeepBucketTable(int size,
                    HeapProfileTable::Allocator alloc,
                    HeapProfileTable::DeAllocator dealloc);
    ~DeepBucketTable();

    // Finds a DeepBucket instance corresponding to the given |bucket|, or
    // creates a new DeepBucket object if it doesn't exist.
    DeepBucket* Lookup(Bucket* bucket,
#if defined(TYPE_PROFILING)
                       const std::type_info* type,
#endif
                       bool is_mmap);

    // Writes stats of the hash table to |buffer| for DumpOrderedProfile.
    void UnparseForStats(TextBuffer* buffer);

    // Writes all buckets for a bucket file with using |buffer|.
    void WriteForBucketFile(const char* prefix,
                            int dump_count,
                            char raw_buffer[],
                            int buffer_size);

    // Resets 'committed_size' members in DeepBucket objects.
    void ResetCommittedSize();

    // Resets all 'is_loggeed' flags in DeepBucket objects.
    void ResetIsLogged();

   private:
    // Adds |add| to a |hash_value| for Lookup.
    inline static void AddToHashValue(uintptr_t add, uintptr_t* hash_value);
    inline static void FinishHashValue(uintptr_t* hash_value);

    DeepBucket** table_;
    size_t table_size_;
    HeapProfileTable::Allocator alloc_;
    HeapProfileTable::DeAllocator dealloc_;
    int bucket_id_;
  };

  class RegionStats {
   public:
    RegionStats(): virtual_bytes_(0), committed_bytes_(0) {}
    ~RegionStats() {}

    // Initializes 'virtual_bytes_' and 'committed_bytes_'.
    void Initialize();

    // Updates itself to contain the tallies of 'virtual_bytes' and
    // 'committed_bytes' in the region from |first_adress| to |last_address|
    // inclusive.
    uint64 Record(
        const MemoryResidenceInfoGetterInterface* memory_residence_info_getter,
        uint64 first_address,
        uint64 last_address);

    // Writes stats of the region into |buffer| with |name|.
    void Unparse(const char* name, TextBuffer* buffer);

    size_t virtual_bytes() const { return virtual_bytes_; }
    size_t committed_bytes() const { return committed_bytes_; }
    void AddToVirtualBytes(size_t additional_virtual_bytes) {
      virtual_bytes_ += additional_virtual_bytes;
    }
    void AddToCommittedBytes(size_t additional_committed_bytes) {
      committed_bytes_ += additional_committed_bytes;
    }
    void AddAnotherRegionStat(const RegionStats& other) {
      virtual_bytes_ += other.virtual_bytes_;
      committed_bytes_ += other.committed_bytes_;
    }

   private:
    size_t virtual_bytes_;
    size_t committed_bytes_;
    DISALLOW_COPY_AND_ASSIGN(RegionStats);
  };

  class GlobalStats {
   public:
    // Snapshots and calculates global stats from /proc/<pid>/maps and pagemap.
    void SnapshotMaps(
        const MemoryResidenceInfoGetterInterface* memory_residence_info_getter,
        DeepHeapProfile* deep_profile,
        TextBuffer* mmap_dump_buffer);

    // Snapshots allocations by malloc and mmap.
    void SnapshotAllocations(DeepHeapProfile* deep_profile);

    // Writes global stats into |buffer|.
    void Unparse(TextBuffer* buffer);

  private:
    // Records both virtual and committed byte counts of malloc and mmap regions
    // as callback functions for AllocationMap::Iterate().
    static void RecordAlloc(const void* pointer,
                            AllocValue* alloc_value,
                            DeepHeapProfile* deep_profile);

    DeepBucket* GetInformationOfMemoryRegion(
        const MemoryRegionMap::RegionIterator& mmap_iter,
        const MemoryResidenceInfoGetterInterface* memory_residence_info_getter,
        DeepHeapProfile* deep_profile);

    // All RegionStats members in this class contain the bytes of virtual
    // memory and committed memory.
    // TODO(dmikurube): These regions should be classified more precisely later
    // for more detailed analysis.
    RegionStats all_[NUMBER_OF_MAPS_REGION_TYPES];

    RegionStats unhooked_[NUMBER_OF_MAPS_REGION_TYPES];

    // Total bytes of malloc'ed regions.
    RegionStats profiled_malloc_;

    // Total bytes of mmap'ed regions.
    RegionStats profiled_mmap_;
  };

  // Writes reformatted /proc/<pid>/maps into a file "|prefix|.<pid>.maps"
  // with using |raw_buffer| of |buffer_size|.
  static void WriteProcMaps(const char* prefix,
                            char raw_buffer[],
                            int buffer_size);

  MemoryResidenceInfoGetterInterface* memory_residence_info_getter_;

  // Process ID of the last dump.  This can change by fork.
  pid_t most_recent_pid_;

  GlobalStats stats_;      // Stats about total memory.
  int dump_count_;         // The number of dumps.
  char* filename_prefix_;  // Output file prefix.

  DeepBucketTable deep_table_;
#endif  // USE_DEEP_HEAP_PROFILE

  HeapProfileTable* heap_profile_;

  DISALLOW_COPY_AND_ASSIGN(DeepHeapProfile);
};

#endif  // BASE_DEEP_HEAP_PROFILE_H_
