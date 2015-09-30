// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/perf_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <bitset>
#include <vector>

#include "base/logging.h"
#include "base/macros.h"

#include "chromiumos-wide-profiling/buffer_reader.h"
#include "chromiumos-wide-profiling/buffer_writer.h"
#include "chromiumos-wide-profiling/compat/string.h"
#include "chromiumos-wide-profiling/file_reader.h"
#include "chromiumos-wide-profiling/sample_info_reader.h"
#include "chromiumos-wide-profiling/utils.h"

namespace quipper {

namespace {

// The type of the number of string data, found in the command line metadata in
// the perf data file.
typedef u32 num_string_data_type;

// Types of the event desc fields that are not found in other structs.
typedef u32 event_desc_num_events;
typedef u32 event_desc_attr_size;
typedef u32 event_desc_num_unique_ids;

// The type of the number of nodes field in NUMA topology.
typedef u32 numa_topology_num_nodes_type;

// A mask that is applied to metadata_mask_ in order to get a mask for
// only the metadata supported by quipper.
const uint32_t kSupportedMetadataMask =
    1 << HEADER_TRACING_DATA |
    1 << HEADER_BUILD_ID |
    1 << HEADER_HOSTNAME |
    1 << HEADER_OSRELEASE |
    1 << HEADER_VERSION |
    1 << HEADER_ARCH |
    1 << HEADER_NRCPUS |
    1 << HEADER_CPUDESC |
    1 << HEADER_CPUID |
    1 << HEADER_TOTAL_MEM |
    1 << HEADER_CMDLINE |
    1 << HEADER_EVENT_DESC |
    1 << HEADER_CPU_TOPOLOGY |
    1 << HEADER_NUMA_TOPOLOGY |
    1 << HEADER_BRANCH_STACK;

// By default, the build ID event has PID = -1.
const uint32_t kDefaultBuildIDEventPid = static_cast<uint32_t>(-1);

// Eight bits in a byte.
size_t BytesToBits(size_t num_bytes) {
  return num_bytes * 8;
}

u8 ReverseByte(u8 x) {
  x = (x & 0xf0) >> 4 | (x & 0x0f) << 4;  // exchange nibbles
  x = (x & 0xcc) >> 2 | (x & 0x33) << 2;  // exchange pairs
  x = (x & 0xaa) >> 1 | (x & 0x55) << 1;  // exchange neighbors
  return x;
}

// If field points to the start of a bitfield padded to len bytes, this
// performs an endian swap of the bitfield, assuming the compiler that produced
// it conforms to the same ABI (bitfield layout is not completely specified by
// the language).
void SwapBitfieldOfBits(u8* field, size_t len) {
  for (size_t i = 0; i < len; i++) {
    field[i] = ReverseByte(field[i]);
  }
}

// The code currently assumes that the compiler will not add any padding to the
// various structs.  These CHECKs make sure that this is true.
void CheckNoEventHeaderPadding() {
  perf_event_header header;
  CHECK_EQ(sizeof(header),
           sizeof(header.type) + sizeof(header.misc) + sizeof(header.size));
}

void CheckNoPerfEventAttrPadding() {
  perf_event_attr attr;
  CHECK_EQ(sizeof(attr),
           (reinterpret_cast<u64>(&attr.__reserved_2) -
            reinterpret_cast<u64>(&attr)) +
           sizeof(attr.__reserved_2));
}

void CheckNoEventTypePadding() {
  perf_trace_event_type event_type;
  CHECK_EQ(sizeof(event_type),
           sizeof(event_type.event_id) + sizeof(event_type.name));
}

void CheckNoBuildIDEventPadding() {
  build_id_event event;
  CHECK_EQ(sizeof(event),
           sizeof(event.header.type) + sizeof(event.header.misc) +
           sizeof(event.header.size) + sizeof(event.pid) +
           sizeof(event.build_id));
}

// Creates/updates a build id event with |build_id| and |filename|.
// Passing "" to |build_id| or |filename| will leave the corresponding field
// unchanged (in which case |event| must be non-null).
// If |event| is empty or is not large enough, a new event will be created.
// Otherwise, updates the fields of the existing event.
// |new_misc| indicates kernel vs user space, and is only used to fill in the
// |header.misc| field of new events.
// In either case, returns a unique pointer to the event containing the updated
// data, or NULL in the case of a failure.
malloced_unique_ptr<build_id_event> CreateOrUpdateBuildID(
    const string& build_id,
    const string& filename,
    uint16_t new_misc,
    malloced_unique_ptr<build_id_event> event) {
  // When creating an event from scratch, build id and filename must be present.
  if (!event && (build_id.empty() || filename.empty()))
    return nullptr;
  size_t new_len = GetUint64AlignedStringLength(
      filename.empty() ? event->filename : filename);

  // If event is null, or we don't have enough memory, allocate more memory, and
  // switch the new pointer with the existing pointer.
  size_t new_size = sizeof(*event) + new_len;
  if (!event || new_size > event->header.size) {
    malloced_unique_ptr<build_id_event> new_event(
        CallocMemoryForBuildID(new_size));

    if (event) {
      // Copy over everything except the filename.
      // It is guaranteed that we are changing the filename - otherwise, the old
      // size and the new size would be equal.
      *new_event = *event;
    } else {
      // Fill in the fields appropriately.
      new_event->header.type = HEADER_BUILD_ID;
      new_event->header.misc = new_misc;
      new_event->pid = kDefaultBuildIDEventPid;
    }
    event = std::move(new_event);
  }

  // Here, event is the pointer to the build_id_event that we are keeping.
  // Update the event's size, build id, and filename.
  if (!build_id.empty() &&
      !StringToHex(build_id, event->build_id, arraysize(event->build_id))) {
    return NULL;
  }

  if (!filename.empty())
    CHECK_GT(snprintf(event->filename, new_len, "%s", filename.c_str()), 0);

  event->header.size = new_size;
  return event;
}

// Reads a CStringWithLength from |data| into |dest| at the current offset.
bool ReadStringFromBuffer(DataReader* data,
                          CStringWithLength* dest) {
  if (!data->ReadUint32(&dest->len)) {
    LOG(ERROR) << "Could not read string length from data.";
    return false;
  }

  if (!data->ReadString(dest->len, &dest->str)) {
    LOG(ERROR) << "Failed to read string from data. len: " << dest->len;
    return false;
  }
  return true;
}

// Writes a CStringWithLength from |src| to |data|.
bool WriteStringToBuffer(const CStringWithLength& src, DataWriter* data) {
  if (data->Tell() + src.len + sizeof(src.len) > data->size()) {
    LOG(ERROR) << "Not enough space to write string.";
    return false;
  }

  if (!data->WriteDataValue(&src.len, sizeof(src.len), "string length") ||
      !data->WriteString(src.str, src.len)) {
    LOG(ERROR) << "Failed to write string.";
    return false;
  }
  return true;
}

// Reads a perf_event_header from |data| and performs byte swapping if
// necessary. Returns true on success, or false if there was an error.
bool ReadPerfEventHeader(DataReader* data, struct perf_event_header* header) {
  if (!data->ReadData(sizeof(struct perf_event_header), header)) {
    LOG(ERROR) << "Error reading perf event header.";
    return false;
  }
  if (data->is_cross_endian()) {
    ByteSwap(&header->type);
    ByteSwap(&header->size);
    ByteSwap(&header->misc);
  }
  return true;
}

// Reads a perf_file_section from |data| and performs byte swapping if
// necessary. Returns true on success, or false if there was an error.
bool ReadPerfFileSection(DataReader* data, struct perf_file_section* section) {
  if (!data->ReadData(sizeof(*section), section)) {
    LOG(ERROR) << "Error reading perf file section info.";
    return false;
  }
  if (data->is_cross_endian()) {
    ByteSwap(&section->offset);
    ByteSwap(&section->size);
  }
  return true;
}

}  // namespace

PerfReader::~PerfReader() {}

void PerfReader::PerfizeBuildIDString(string* build_id) {
  build_id->resize(kBuildIDStringLength, '0');
}

void PerfReader::TrimZeroesFromBuildIDString(string* build_id) {
  const size_t kPaddingSize = 8;
  const string kBuildIDPadding = string(kPaddingSize, '0');

  // Remove kBuildIDPadding from the end of build_id until we cannot remove any
  // more. The build ID string can be reduced down to an empty string. This
  // could happen if the file did not have a build ID but was given a build ID
  // of all zeroes. The empty build ID string would reflect the original lack of
  // build ID.
  while (build_id->size() >= kPaddingSize &&
         build_id->substr(build_id->size() - kPaddingSize) == kBuildIDPadding) {
    build_id->resize(build_id->size() - kPaddingSize);
  }
}

bool PerfReader::ReadFile(const string& filename) {
  FileReader reader(filename);
  if (!reader.IsOpen()) {
    LOG(ERROR) << "Unable to open file " << filename;
    return false;
  }
  return ReadFromData(&reader);
}

bool PerfReader::ReadFromVector(const std::vector<char>& data) {
  return ReadFromPointer(data.data(), data.size());
}

bool PerfReader::ReadFromString(const string& str) {
  return ReadFromPointer(str.data(), str.size());
}

bool PerfReader::ReadFromPointer(const char* data, size_t size) {
  BufferReader buffer(data, size);
  return ReadFromData(&buffer);
}

bool PerfReader::ReadFromData(DataReader* data) {
  if (data->size() == 0) {
    LOG(ERROR) << "Input data is empty!";
    return false;
  }
  if (!ReadHeader(data))
    return false;

  // Check if it is normal perf data.
  if (header_.size == sizeof(header_)) {
    DLOG(INFO) << "Perf data is in normal format.";

    // Make sure sections are within the size of the file. This check prevents
    // more obscure messages later when attempting to read from one of these
    // sections.
    if (header_.attrs.offset + header_.attrs.size > data->size()) {
      LOG(ERROR) << "Header says attrs section ends at "
                 << header_.attrs.offset + header_.attrs.size
                 << " bytes, which is larger than perf data size of "
                 << data->size() << " bytes.";
      return false;
    }
    if (header_.data.offset + header_.data.size > data->size()) {
      LOG(ERROR) << "Header says data section ends at "
                 << header_.data.offset + header_.data.size
                 << " bytes, which is larger than perf data size of "
                 << data->size() << " bytes.";
      return false;
    }
    if (header_.event_types.offset + header_.event_types.size > data->size()) {
      LOG(ERROR) << "Header says event_types section ends at "
                 << header_.event_types.offset + header_.event_types.size
                 << " bytes, which is larger than perf data size of "
                 << data->size() << " bytes.";
      return false;
    }

    metadata_mask_ = header_.adds_features[0];
    if (!(metadata_mask_ & (1 << HEADER_EVENT_DESC))) {
      // Prefer to read attrs and event names from HEADER_EVENT_DESC metadata if
      // available. event_types section of perf.data is obsolete, but use it as
      // a fallback:
      if (!(ReadAttrsSection(data) && ReadEventTypesSection(data)))
        return false;
    }

    if (!(ReadDataSection(data) && ReadMetadata(data)))
      return false;

    // We can construct HEADER_EVENT_DESC from attrs and event types.
    // NB: Can't set this before ReadMetadata(), or it may misread the metadata.
    metadata_mask_ |= (1 << HEADER_EVENT_DESC);

    return true;
  }

  // Otherwise it is piped data.
  if (piped_header_.size != sizeof(piped_header_)) {
    LOG(ERROR) << "Expecting piped data format, but header size "
               << piped_header_.size << " does not match expected size "
               << sizeof(piped_header_);
    return false;
  }

  return ReadPipedData(data);
}

bool PerfReader::WriteFile(const string& filename) {
  std::vector<char> data;
  return WriteToVector(&data) && WriteDataToFile(data, filename);
}

bool PerfReader::WriteToVector(std::vector<char>* data) {
  data->resize(GetSize());
  return WriteToPointerWithoutCheckingSize(&data->at(0), data->size());
}

bool PerfReader::WriteToString(string* str) {
  str->resize(GetSize());
  return WriteToPointerWithoutCheckingSize(&str->at(0), str->size());
}

bool PerfReader::WriteToPointer(char* buffer, size_t size) {
  size_t required_size = GetSize();
  if (size < required_size) {
    LOG(ERROR) << "Buffer is too small - buffer size is " << size
               << " and required size is " << required_size;
    return false;
  }
  return WriteToPointerWithoutCheckingSize(buffer, size);
}

bool PerfReader::WriteToPointerWithoutCheckingSize(char* buffer, size_t size) {
  BufferWriter data(buffer, size);
  struct perf_file_header header;
  GenerateHeader(&header);

  return
      WriteHeader(header, &data) &&
      WriteAttrs(header, &data) &&
      WriteEventTypes(header, &data) &&
      WriteData(header, &data) &&
      WriteMetadata(header, &data);
}

size_t PerfReader::GetSize() const {
  struct perf_file_header header;
  GenerateHeader(&header);

  size_t total_size = 0;
  total_size += header.size;
  total_size += header.attrs.size;
  total_size += header.event_types.size;
  total_size += header.data.size;
  // Add the ID info, whose size is not explicitly included in the header.
  for (size_t i = 0; i < attrs_.size(); ++i)
    total_size += attrs_[i].ids.size() * sizeof(attrs_[i].ids[0]);

  // Additional info about metadata.  See WriteMetadata for explanation.
  total_size += GetNumSupportedMetadata() * sizeof(struct perf_file_section);

  // Add the sizes of the various metadata.
  total_size += tracing_data_.size();
  total_size += GetBuildIDMetadataSize();
  total_size += GetStringMetadataSize();
  total_size += GetUint32MetadataSize();
  total_size += GetUint64MetadataSize();
  total_size += GetEventDescMetadataSize();
  total_size += GetCPUTopologyMetadataSize();
  total_size += GetNUMATopologyMetadataSize();
  return total_size;
}

void PerfReader::GenerateHeader(struct perf_file_header* header) const {
  // This is the order of the input perf file contents in normal mode:
  // 1. Header
  // 2. Attribute IDs (pointed to by attr.ids.offset)
  // 3. Attributes
  // 4. Event types
  // 5. Data
  // 6. Metadata

  // Compute offsets in the above order.
  CheckNoEventHeaderPadding();
  memset(header, 0, sizeof(*header));
  header->magic = kPerfMagic;
  header->size = sizeof(*header);
  header->attr_size = sizeof(perf_file_attr);
  header->attrs.size = header->attr_size * attrs_.size();
  for (size_t i = 0; i < events_.size(); i++)
    header->data.size += events_[i]->header.size;
  header->event_types.size = HaveEventNames() ?
      (attrs_.size() * sizeof(perf_trace_event_type)) : 0;

  u64 current_offset = 0;
  current_offset += header->size;
  for (size_t i = 0; i < attrs_.size(); i++)
    current_offset += sizeof(attrs_[i].ids[0]) * attrs_[i].ids.size();
  header->attrs.offset = current_offset;
  current_offset += header->attrs.size;
  header->event_types.offset = current_offset;
  current_offset += header->event_types.size;

  header->data.offset = current_offset;

  // Construct the header feature bits.
  memset(&header->adds_features, 0, sizeof(header->adds_features));
  // The following code makes the assumption that all feature bits are in the
  // first word of |adds_features|.  If the perf data format changes and the
  // assumption is no longer valid, this CHECK will fail, at which point the
  // below code needs to be updated.  For now, sticking to that assumption keeps
  // the code simple.
  // This assumption is also used when reading metadata, so that code
  // will also have to be updated if this CHECK starts to fail.
  CHECK_LE(static_cast<size_t>(HEADER_LAST_FEATURE),
           BytesToBits(sizeof(header->adds_features[0])));
  header->adds_features[0] |= metadata_mask_ & kSupportedMetadataMask;
}

bool PerfReader::InjectBuildIDs(
    const std::map<string, string>& filenames_to_build_ids) {
  metadata_mask_ |= (1 << HEADER_BUILD_ID);
  std::set<string> updated_filenames;
  // Inject new build ID's for existing build ID events.
  for (malloced_unique_ptr<build_id_event>& event : build_id_events_) {
    string filename = event->filename;
    if (filenames_to_build_ids.find(filename) == filenames_to_build_ids.end())
      continue;

    string build_id = filenames_to_build_ids.at(filename);
    PerfizeBuildIDString(&build_id);
    event = CreateOrUpdateBuildID(build_id, "", 0, std::move(event));
    updated_filenames.insert(filename);
  }

  // For files with no existing build ID events, create new build ID events.
  // This requires a lookup of all MMAP's to determine the |misc| field of each
  // build ID event.
  std::map<string, uint16_t> filename_to_misc;
  for (size_t i = 0; i < events_.size(); ++i) {
    const event_t& event = *events_[i];
    if (event.header.type == PERF_RECORD_MMAP)
      filename_to_misc[event.mmap.filename] = event.header.misc;
    if (event.header.type == PERF_RECORD_MMAP2)
      filename_to_misc[event.mmap2.filename] = event.header.misc;
  }

  std::map<string, string>::const_iterator it;
  for (it = filenames_to_build_ids.begin();
       it != filenames_to_build_ids.end();
       ++it) {
    const string& filename = it->first;
    if (updated_filenames.find(filename) != updated_filenames.end())
      continue;

    // Determine the misc field.
    uint16_t new_misc = PERF_RECORD_MISC_KERNEL;
    std::map<string, uint16_t>::const_iterator misc_iter =
        filename_to_misc.find(filename);
    if (misc_iter != filename_to_misc.end())
      new_misc = misc_iter->second;

    string build_id = it->second;
    PerfizeBuildIDString(&build_id);
    malloced_unique_ptr<build_id_event> event =
        CreateOrUpdateBuildID(build_id, filename, new_misc, nullptr);
    CHECK(event);
    build_id_events_.push_back(std::move(event));
  }

  return true;
}

bool PerfReader::Localize(
    const std::map<string, string>& build_ids_to_filenames) {
  std::map<string, string> perfized_build_ids_to_filenames;
  std::map<string, string>::const_iterator it;
  for (it = build_ids_to_filenames.begin();
       it != build_ids_to_filenames.end();
       ++it) {
    string build_id = it->first;
    PerfizeBuildIDString(&build_id);
    perfized_build_ids_to_filenames[build_id] = it->second;
  }

  std::map<string, string> filename_map;
  for (malloced_unique_ptr<build_id_event>& event : build_id_events_) {
    string build_id = HexToString(event->build_id, kBuildIDArraySize);
    if (perfized_build_ids_to_filenames.find(build_id) ==
        perfized_build_ids_to_filenames.end()) {
      continue;
    }

    string new_name = perfized_build_ids_to_filenames.at(build_id);
    filename_map[string(event->filename)] = new_name;
    event = CreateOrUpdateBuildID("", new_name, 0, std::move(event));
    CHECK(event);
  }

  LocalizeUsingFilenames(filename_map);
  return true;
}

bool PerfReader::LocalizeUsingFilenames(
    const std::map<string, string>& filename_map) {
  LocalizeMMapFilenames(filename_map);
  for (malloced_unique_ptr<build_id_event>& event : build_id_events_) {
    string old_name = event->filename;

    if (filename_map.find(event->filename) != filename_map.end()) {
      const string& new_name = filename_map.at(old_name);
      event = CreateOrUpdateBuildID("", new_name, 0, std::move(event));
      CHECK(event);
    }
  }
  return true;
}

void PerfReader::GetFilenames(std::vector<string>* filenames) const {
  std::set<string> filename_set;
  GetFilenamesAsSet(&filename_set);
  filenames->clear();
  filenames->insert(filenames->begin(), filename_set.begin(),
                    filename_set.end());
}

void PerfReader::GetFilenamesAsSet(std::set<string>* filenames) const {
  filenames->clear();
  for (size_t i = 0; i < events_.size(); ++i) {
    const event_t& event = *events_[i];
    if (event.header.type == PERF_RECORD_MMAP)
      filenames->insert(event.mmap.filename);
    if (event.header.type == PERF_RECORD_MMAP2)
      filenames->insert(event.mmap2.filename);
  }
}

void PerfReader::GetFilenamesToBuildIDs(
    std::map<string, string>* filenames_to_build_ids) const {
  filenames_to_build_ids->clear();
  for (size_t i = 0; i < build_id_events_.size(); ++i) {
    const build_id_event& event = *build_id_events_[i].get();
    string build_id = HexToString(event.build_id, kBuildIDArraySize);
    (*filenames_to_build_ids)[event.filename] = build_id;
  }
}

bool PerfReader::ReadPerfSampleInfo(const event_t& event,
                                    struct perf_sample* sample) const {
  if (sample_info_reader_.get() == nullptr)
    return false;
  return sample_info_reader_->ReadPerfSampleInfo(event, sample);
}

bool PerfReader::WritePerfSampleInfo(const perf_sample& sample,
                                     event_t* event) const {
  if (sample_info_reader_.get() == nullptr)
    return false;
  return sample_info_reader_->WritePerfSampleInfo(sample, event);
}

bool PerfReader::ReadHeader(DataReader* data) {
  CheckNoEventHeaderPadding();
  // The header is the first thing to be read. Don't use SeekSet(0) because it
  // doesn't make sense for piped files. Instead, verify that the reader points
  // to the start of the data.
  CHECK_EQ(0U, data->Tell());
  if (!data->ReadUint64(&piped_header_.magic)) {
    LOG(ERROR) << "Error reading header magic number.";
    return false;
  }

  if (piped_header_.magic != kPerfMagic &&
      piped_header_.magic != bswap_64(kPerfMagic)) {
    LOG(ERROR) << "Read wrong magic. Expected: 0x" << std::hex << kPerfMagic
               << " or 0x" << std::hex << bswap_64(kPerfMagic)
               << " Got: 0x" << std::hex << piped_header_.magic;
    return false;
  }
  data->set_is_cross_endian(piped_header_.magic != kPerfMagic);

  if (!data->ReadUint64(&piped_header_.size)) {
    LOG(ERROR) << "Error reading header size.";
    return false;
  }

  CHECK_EQ(data->Tell(), sizeof(piped_header_));

  // Header can be a piped header.
  if (piped_header_.size == sizeof(piped_header_))
    return true;

  // Read as a non-piped header.
  if (!data->ReadUint64(&header_.attr_size)) {
    LOG(ERROR) << "Error reading header::attr_size.";
    return false;
  }
  if (!ReadPerfFileSection(data, &header_.attrs) ||
      !ReadPerfFileSection(data, &header_.data) ||
      !ReadPerfFileSection(data, &header_.event_types)) {
    LOG(ERROR) << "Error reading header file section info.";
    return false;
  }

  const size_t features_size = sizeof(header_.adds_features);
  CHECK_EQ(data->Tell(), sizeof(header_) - features_size);

  if (!data->ReadData(features_size, header_.adds_features)) {
    LOG(ERROR) << "Error reading header::adds_features.";
    return false;
  }
  // Byte-swapping |adds_features| is tricky. It is defined as an array of
  // unsigned longs, which can vary between architectures. However, the overall
  // size of the array in bytes is fixed.
  //
  // According to perf's perf_file_header__read() function, the hostname feature
  // should always be set. Try byte-swapping as uint64s first and check the
  // hostname bit. If it's not set, then try swapping as uint32s. This is
  // similar to the algorithm used in perf.
  if (data->is_cross_endian()) {
    static_assert(sizeof(header_.adds_features[0]) == sizeof(uint32_t) ||
                  sizeof(header_.adds_features[0]) == sizeof(uint64_t),
                  "|header_.adds_features| must be defined as an array of "
                  "either 32-bit or 64-bit words.");

    uint64_t features64 = 0;
    // Some compilers will complain if we directly cast |header_.adds_features|
    // to a uint64_t*. Instead, determine the first uint64_t without using
    // pointer aliasing.
    if (sizeof(header_.adds_features[0]) == sizeof(uint64_t)) {
      features64 = bswap_64(header_.adds_features[0]);
    } else {
      // If the native |adds_features| is composed of 32-bit words, swap the
      // byte order of each word and then swap their positions to create a
      // 64-bit word.
      features64 =
          static_cast<uint64_t>(bswap_32(header_.adds_features[0])) << 32;
      features64 |= bswap_32(header_.adds_features[1]);
    }
    if (features64 & (1 << HEADER_HOSTNAME)) {
      for (size_t i = 0; i < features_size / sizeof(uint64_t); ++i)
        ByteSwap(reinterpret_cast<uint64_t*>(header_.adds_features) + i);
    } else {
      for (size_t i = 0; i < features_size / sizeof(uint32_t); ++i)
        ByteSwap(reinterpret_cast<uint32_t*>(header_.adds_features) + i);
    }
  }

  return true;
}

bool PerfReader::ReadAttrsSection(DataReader* data) {
  size_t num_attrs = header_.attrs.size / header_.attr_size;
  if (header_.attrs.size % header_.attr_size != 0) {
    LOG(ERROR) << "Total size of attrs " << header_.attrs.size
               << " is not a multiple of attr size " << header_.attr_size;
  }
  data->SeekSet(header_.attrs.offset);
  for (size_t i = 0; i < num_attrs; i++) {
    if (!ReadAttr(data))
      return false;
  }
  return true;
}

bool PerfReader::ReadAttr(DataReader* data) {
  PerfFileAttr attr;
  if (!ReadEventAttr(data, &attr.attr))
    return false;

  perf_file_section ids;
  if (!ReadPerfFileSection(data, &ids))
    return false;

  // The ID may be stored at a different location in the file than where we're
  // currently reading.
  size_t saved_offset = data->Tell();
  data->SeekSet(ids.offset);

  size_t num_ids = ids.size / sizeof(decltype(attr.ids)::value_type);
  if (!ReadUniqueIDs(data, num_ids, &attr.ids))
    return false;
  data->SeekSet(saved_offset);
  attrs_.push_back(attr);
  return true;
}

bool PerfReader::ReadEventAttr(DataReader* data, perf_event_attr* attr) {
  CheckNoPerfEventAttrPadding();
  *attr = {0};

  static_assert(
      offsetof(struct perf_event_attr, size) == sizeof(perf_event_attr::type),
      "type and size should be the first to fields of perf_event_attr");

  if (!data->ReadUint32(&attr->type) || !data->ReadUint32(&attr->size)) {
    LOG(ERROR) << "Error reading event attr type and size.";
    return false;
  }

  // Now read the rest of the attr struct.
  const size_t attr_offset = sizeof(attr->type) + sizeof(attr->size);
  const size_t attr_readable_size =
      std::min(static_cast<size_t>(attr->size), sizeof(*attr));
  if (!data->ReadDataValue(attr_readable_size - attr_offset, "attribute",
                           reinterpret_cast<char*>(attr) + attr_offset)) {
    return false;
  }
  data->SeekSet(data->Tell() + attr->size - attr_readable_size);

  if (data->is_cross_endian()) {
    // Depending on attr->size, some of these might not have actually been
    // read. This is okay: they are zero.
    ByteSwap(&attr->type);
    ByteSwap(&attr->size);
    ByteSwap(&attr->config);
    ByteSwap(&attr->sample_period);
    ByteSwap(&attr->sample_type);
    ByteSwap(&attr->read_format);

    // NB: This will also reverse precise_ip : 2 as if it was two fields:
    auto *const bitfield_start = &attr->read_format + 1;
    SwapBitfieldOfBits(reinterpret_cast<u8*>(bitfield_start), sizeof(u64));
    // ... So swap it back:
    const auto tmp = attr->precise_ip;
    attr->precise_ip = (tmp & 0x2) >> 1 | (tmp & 0x1) << 1;

    ByteSwap(&attr->wakeup_events);  // union with wakeup_watermark
    ByteSwap(&attr->bp_type);
    ByteSwap(&attr->bp_addr);        // union with config1
    ByteSwap(&attr->bp_len);         // union with config2
    ByteSwap(&attr->branch_sample_type);
    ByteSwap(&attr->sample_regs_user);
    ByteSwap(&attr->sample_stack_user);
  }

  // The actual perf_event_attr data size might be different from the size of
  // the struct definition.  Check against perf_event_attr's |size| field.
  attr->size = sizeof(*attr);

  // Assign sample type if it hasn't been assigned, otherwise make sure all
  // subsequent attributes have the same sample type bits set.
  if (sample_type_ == 0) {
    sample_type_ = attr->sample_type;
  } else {
    CHECK_EQ(sample_type_, attr->sample_type)
        << "Event type sample format does not match sample format of other "
        << "event type.";
  }

  if (read_format_ == 0) {
    read_format_ = attr->read_format;
  } else {
    CHECK_EQ(read_format_, attr->read_format)
        << "Event type read format does not match read format of other event "
        << "types.";
  }
  if (sample_info_reader_.get() == nullptr) {
    sample_info_reader_.reset(
        new SampleInfoReader(*attr, false /* read_cross_endian */));
  }

  return true;
}

bool PerfReader::ReadUniqueIDs(DataReader* data, size_t num_ids,
                               std::vector<u64>* ids) {
  ids->resize(num_ids);
  for (u64& id : *ids) {
    if (!data->ReadUint64(&id)) {
      LOG(ERROR) << "Error reading unique ID.";
      return false;
    }
  }
  return true;
}

bool PerfReader::ReadEventTypesSection(DataReader* data) {
  size_t num_event_types = header_.event_types.size /
      sizeof(struct perf_trace_event_type);
  if (num_event_types == 0) {
    // Not available.
    return true;
  }
  CHECK_EQ(attrs_.size(), num_event_types);
  CHECK_EQ(sizeof(perf_trace_event_type) * num_event_types,
           header_.event_types.size);
  data->SeekSet(header_.event_types.offset);
  for (size_t i = 0; i < num_event_types; ++i) {
    if (!ReadEventType(data, i, 0))
      return false;
  }
  return true;
}

bool PerfReader::ReadEventType(DataReader* data,
                               size_t attr_idx, size_t event_size) {
  CheckNoEventTypePadding();
  decltype(perf_trace_event_type::event_id) event_id;

  if (!data->ReadUint64(&event_id)) {
    LOG(ERROR) << "Error reading event ID.";
    return false;
  }

  size_t event_name_len;
  if (event_size == 0) {  // Not in an event.
    event_name_len = sizeof(perf_trace_event_type::name);
  } else {
    event_name_len = event_size - sizeof(perf_event_header) - sizeof(event_id);
  }
  string event_name;
  if (!data->ReadString(event_name_len, &event_name)) {
    LOG(ERROR) << "Not enough data left in data to read event name.";
    return false;
  }

  if (attr_idx >= attrs_.size()) {
    LOG(ERROR) << "Too many event types, or attrs not read yet!";
    return false;
  }
  if (event_id != attrs_[attr_idx].attr.config) {
    LOG(ERROR) << "event_id for perf_trace_event_type (" << event_id << ") "
               << "does not match attr.config ("
               << attrs_[attr_idx].attr.config << ")";
    return false;
  }
  attrs_[attr_idx].name = event_name;

  return true;
}

bool PerfReader::ReadDataSection(DataReader* data) {
  u64 data_remaining_bytes = header_.data.size;
  data->SeekSet(header_.data.offset);
  while (data_remaining_bytes != 0) {
    perf_event_header header;

    // Read the header to determine the size of the event.
    if (!ReadPerfEventHeader(data, &header)) {
      LOG(ERROR) << "Error reading event header from data section.";
      return false;
    }

    // Allocate memory for event and copy over the header.
    malloced_unique_ptr<event_t> event(CallocMemoryForEvent(header.size));
    event->header = header;

    // Read the rest of the event data.
    if (!data->ReadDataValue(header.size - sizeof(header), "rest of event",
                             &event->header + 1)) {
      return false;
    }
    MaybeSwapEventFields(event.get(), data->is_cross_endian());
    events_.push_back(std::move(event));

    data_remaining_bytes -= header.size;
  }

  DLOG(INFO) << "Number of events stored: "<< events_.size();
  return true;
}

bool PerfReader::ReadMetadata(DataReader* data) {
  // Metadata comes after the event data.
  data->SeekSet(header_.data.offset + header_.data.size);

  // Read the (offset, size) pairs of all the metadata elements. Note that this
  // takes into account all present metadata types, not just the ones included
  // in |kSupportedMetadataMask|. If a metadata type is not supported, it is
  // skipped over.
  std::vector<struct perf_file_section> sections(GetNumBits(metadata_mask_));
  for (struct perf_file_section& section : sections) {
    if (!ReadPerfFileSection(data, &section)) {
      LOG(ERROR) << "Error reading metadata entry info.";
      return false;
    }
  }

  auto section_iter = sections.begin();
  for (u32 type = HEADER_FIRST_FEATURE; type != HEADER_LAST_FEATURE; ++type) {
    if ((metadata_mask_ & (1 << type)) == 0)
      continue;
    data->SeekSet(section_iter->offset);
    u64 size = section_iter->size;

    switch (type) {
    case HEADER_TRACING_DATA:
      if (!ReadTracingMetadata(data, size))
        return false;
      break;
    case HEADER_BUILD_ID:
      if (!ReadBuildIDMetadata(data, size))
        return false;
      break;
    case HEADER_HOSTNAME:
    case HEADER_OSRELEASE:
    case HEADER_VERSION:
    case HEADER_ARCH:
    case HEADER_CPUDESC:
    case HEADER_CPUID:
    case HEADER_CMDLINE:
      if (!ReadStringMetadata(data, type, size))
        return false;
      break;
    case HEADER_NRCPUS:
      if (!ReadUint32Metadata(data, type, size))
        return false;
      break;
    case HEADER_TOTAL_MEM:
      if (!ReadUint64Metadata(data, type, size))
        return false;
      break;
    case HEADER_EVENT_DESC:
      if (!ReadEventDescMetadata(data, type, size))
        return false;
      break;
    case HEADER_CPU_TOPOLOGY:
      if (!ReadCPUTopologyMetadata(data, type, size))
        return false;
      break;
    case HEADER_NUMA_TOPOLOGY:
      if (!ReadNUMATopologyMetadata(data, type, size))
        return false;
      break;
    case HEADER_BRANCH_STACK:
      break;
    default:
      LOG(INFO) << "Unsupported metadata type, skipping: " << type;
      break;
    }
    ++section_iter;
  }

  return true;
}

bool PerfReader::ReadBuildIDMetadata(DataReader* data, size_t size) {
  CheckNoBuildIDEventPadding();
  while (size > 0) {
    // Make sure there is enough data for everything but the filename.
    perf_event_header build_id_header;
    if (!ReadPerfEventHeader(data, &build_id_header)) {
      LOG(ERROR) << "Error reading build ID header.";
      return false;
    }

    if (!ReadBuildIDMetadataWithoutHeader(data, build_id_header))
      return false;
    size -= build_id_header.size;
  }

  return true;
}

bool PerfReader::ReadBuildIDMetadataWithoutHeader(
    DataReader* data, const perf_event_header& header) {
  // Allocate memory for the event.
  malloced_unique_ptr<build_id_event>
      event(CallocMemoryForBuildID(header.size));
  event->header = header;

  // Make sure there is enough data for the rest of the event.
  if (!data->ReadDataValue(header.size - sizeof(header),
                           "rest of build ID event", &event->header + 1)) {
    LOG(ERROR) << "Not enough bytes to read build id event";
    return false;
  }
  if (data->is_cross_endian())
    ByteSwap(&event->pid);

  // Perf tends to use more space than necessary, so fix the size.
  event->header.size =
      sizeof(*event) + GetUint64AlignedStringLength(event->filename);
  build_id_events_.push_back(std::move(event));
  return true;
}

bool PerfReader::ReadStringMetadata(DataReader* data, u32 type, size_t size) {
  PerfStringMetadata str_data;
  str_data.type = type;

  // Skip the number of string data if it is present.
  if (NeedsNumberOfStringData(type)) {
    num_string_data_type count;
    if (!data->ReadUint32(&count)) {
      LOG(ERROR) << "Error reading string count.";
      return false;
    }
    size -= sizeof(count);
  }

  while (size > 0) {
    CStringWithLength single_string;
    if (!ReadStringFromBuffer(data, &single_string))
      return false;
    str_data.data.push_back(single_string);
    size -= (sizeof(single_string.len) + single_string.len);
  }

  string_metadata_.push_back(str_data);
  return true;
}

bool PerfReader::ReadUint32Metadata(DataReader* data, u32 type, size_t size) {
  PerfUint32Metadata uint32_data;
  uint32_data.type = type;

  while (size > 0) {
    uint32_t item;
    if (!data->ReadUint32(&item)) {
      LOG(ERROR) << "Error reading uint32 metadata";
      return false;
    }

    uint32_data.data.push_back(item);
    size -= sizeof(item);
  }

  uint32_metadata_.push_back(uint32_data);
  return true;
}

bool PerfReader::ReadUint64Metadata(DataReader* data, u32 type, size_t size) {
  PerfUint64Metadata uint64_data;
  uint64_data.type = type;

  while (size > 0) {
    uint64_t item;
    if (!data->ReadUint64(&item)) {
      LOG(ERROR) << "Error reading uint64 metadata";
      return false;
    }

    uint64_data.data.push_back(item);
    size -= sizeof(item);
  }

  uint64_metadata_.push_back(uint64_data);
  return true;
}

// TODO(cwp-team): Move this to match the order in the header file.
bool PerfReader::ReadEventDescMetadata(DataReader* data, u32 type,
                                       size_t size) {
  // Structure:
  // u32 nr_events
  // u32 sizeof(perf_event_attr)
  // foreach event (nr_events):
  //   struct perf_event_attr
  //   u32 nr_ids
  //   event name (len & string, 64-bit padded)
  //   u64 ids[nr_ids]

  u32 nr_events;
  if (!data->ReadUint32(&nr_events)) {
    LOG(ERROR) << "Error reading event_desc nr_events.";
    return false;
  }

  u32 attr_size;
  if (!data->ReadUint32(&attr_size)) {
    LOG(ERROR) << "Error reading event_desc attr_size.";
    return false;
  }

  attrs_.clear();
  attrs_.resize(nr_events);

  for (u32 i = 0; i < nr_events; i++) {
    if (!ReadEventAttr(data, &attrs_[i].attr))
      return false;

    u32 nr_ids;
    if (!data->ReadUint32(&nr_ids)) {
      LOG(ERROR) << "Error reading event_desc nr_ids.";
      return false;
    }

    CStringWithLength event_name;
    if (!ReadStringFromBuffer(data, &event_name))
      return false;
    attrs_[i].name = event_name.str;

    std::vector<u64> &ids = attrs_[i].ids;
    ids.resize(nr_ids);
    for (u64& id : ids) {
      if (!data->ReadUint64(&id)) {
        LOG(ERROR) << "Error reading ID value for attr #" << i;
        return false;
      }
    }
  }
  return true;
}

bool PerfReader::ReadCPUTopologyMetadata(DataReader* data, u32 type,
                                         size_t size) {
  num_siblings_type num_core_siblings;
  if (!data->ReadUint32(&num_core_siblings)) {
    LOG(ERROR) << "Error reading num core siblings.";
    return false;
  }

  cpu_topology_.core_siblings.resize(num_core_siblings);
  for (size_t i = 0; i < num_core_siblings; ++i) {
    if (!ReadStringFromBuffer(data, &cpu_topology_.core_siblings[i]))
      return false;
  }

  num_siblings_type num_thread_siblings;
  if (!data->ReadUint32(&num_thread_siblings)) {
    LOG(ERROR) << "Error reading num core siblings.";
    return false;
  }

  cpu_topology_.thread_siblings.resize(num_thread_siblings);
  for (size_t i = 0; i < num_thread_siblings; ++i) {
    if (!ReadStringFromBuffer(data, &cpu_topology_.thread_siblings[i]))
      return false;
  }

  return true;
}

bool PerfReader::ReadNUMATopologyMetadata(DataReader* data, u32 type,
                                          size_t size) {
  numa_topology_num_nodes_type num_nodes;
  if (!data->ReadUint32(&num_nodes)) {
    LOG(ERROR) << "Error reading NUMA topology num nodes.";
    return false;
  }

  for (size_t i = 0; i < num_nodes; ++i) {
    PerfNodeTopologyMetadata node;
    if (!data->ReadUint32(&node.id) ||
        !data->ReadUint64(&node.total_memory) ||
        !data->ReadUint64(&node.free_memory) ||
        !ReadStringFromBuffer(data, &node.cpu_list)) {
      LOG(ERROR) << "Error reading NUMA topology info for node #" << i;
      return false;
    }
    numa_topology_.push_back(node);
  }
  return true;
}

// TODO(cwp-team): Move this to match the order in the header file.
bool PerfReader::ReadTracingMetadata(DataReader* data, size_t size) {
  tracing_data_.resize(size);
  return data->ReadDataValue(tracing_data_.size(), "tracing_data",
                             tracing_data_.data());
}

bool PerfReader::ReadPipedData(DataReader* data) {
  // The piped data comes right after the file header.
  CHECK_EQ(piped_header_.size, data->Tell());
  bool result = true;
  size_t num_event_types = 0;

  metadata_mask_ = 0;
  CheckNoEventHeaderPadding();

  while (result && data->Tell() < data->size()) {
    perf_event_header header;
    if (!ReadPerfEventHeader(data, &header)) {
      LOG(ERROR) << "Error reading event header.";
      break;
    }

    if (header.size == 0) {
      // Avoid an infinite loop.
      LOG(ERROR) << "Event size is zero. Type: " << header.type;
      return false;
    }

    // Compute the size of the post-header part of the event data.
    size_t size_without_header = header.size - sizeof(header);

    if (header.type < PERF_RECORD_MAX) {
      // Allocate space for an event struct based on the size in the header.
      // Don't blindly allocate the entire event_t because it is a
      // variable-sized type that may include data beyond what's nominally
      // declared in its definition.
      malloced_unique_ptr<event_t> event(CallocMemoryForEvent(header.size));
      event->header = header;

      // Read the rest of the event data.
      if (!data->ReadDataValue(size_without_header, "rest of piped event",
                               &event->header + 1)) {
        break;
      }
      MaybeSwapEventFields(event.get(), data->is_cross_endian());
      events_.push_back(std::move(event));

      continue;
    }

    switch (header.type) {
    case PERF_RECORD_HEADER_ATTR:
      result = ReadAttrEventBlock(data, size_without_header);
      break;
    case PERF_RECORD_HEADER_EVENT_TYPE:
      result = ReadEventType(data, num_event_types++, header.size);
      break;
    case PERF_RECORD_HEADER_EVENT_DESC:
      metadata_mask_ |= (1 << HEADER_EVENT_DESC);
      result = ReadEventDescMetadata(data, HEADER_EVENT_DESC,
                                     size_without_header);
      break;
    case PERF_RECORD_HEADER_TRACING_DATA:
      metadata_mask_ |= (1 << HEADER_TRACING_DATA);
      {
        // TRACING_DATA's header.size is a lie. It is the size of only the event
        // struct. The size of the data is in the event struct, and followed
        // immediately by the tracing header data.
        decltype(tracing_data_event::size) size = 0;
        if (!data->ReadUint32(&size)) {
          LOG(ERROR) << "Error reading tracing data size.";
          return false;
        }
        result = ReadTracingMetadata(data, size);
      }
      break;
    case PERF_RECORD_HEADER_BUILD_ID:
      metadata_mask_ |= (1 << HEADER_BUILD_ID);
      result = ReadBuildIDMetadataWithoutHeader(data, header);
      break;
    case PERF_RECORD_HEADER_HOSTNAME:
      metadata_mask_ |= (1 << HEADER_HOSTNAME);
      result = ReadStringMetadata(data, HEADER_HOSTNAME, size_without_header);
      break;
    case PERF_RECORD_HEADER_OSRELEASE:
      metadata_mask_ |= (1 << HEADER_OSRELEASE);
      result = ReadStringMetadata(data, HEADER_OSRELEASE, size_without_header);
      break;
    case PERF_RECORD_HEADER_VERSION:
      metadata_mask_ |= (1 << HEADER_VERSION);
      result = ReadStringMetadata(data, HEADER_VERSION, size_without_header);
      break;
    case PERF_RECORD_HEADER_ARCH:
      metadata_mask_ |= (1 << HEADER_ARCH);
      result = ReadStringMetadata(data, HEADER_ARCH, size_without_header);
      break;
    case PERF_RECORD_HEADER_CPUDESC:
      metadata_mask_ |= (1 << HEADER_CPUDESC);
      result = ReadStringMetadata(data, HEADER_CPUDESC, size_without_header);
      break;
    case PERF_RECORD_HEADER_CPUID:
      metadata_mask_ |= (1 << HEADER_CPUID);
      result = ReadStringMetadata(data, HEADER_CPUID, size_without_header);
      break;
    case PERF_RECORD_HEADER_CMDLINE:
      metadata_mask_ |= (1 << HEADER_CMDLINE);
      result = ReadStringMetadata(data, HEADER_CMDLINE, size_without_header);
      break;
    case PERF_RECORD_HEADER_NRCPUS:
      metadata_mask_ |= (1 << HEADER_NRCPUS);
      result = ReadUint32Metadata(data, HEADER_NRCPUS, size_without_header);
      break;
    case PERF_RECORD_HEADER_TOTAL_MEM:
      metadata_mask_ |= (1 << HEADER_TOTAL_MEM);
      result = ReadUint64Metadata(data, HEADER_TOTAL_MEM, size_without_header);
      break;
    case PERF_RECORD_HEADER_CPU_TOPOLOGY:
      metadata_mask_ |= (1 << HEADER_CPU_TOPOLOGY);
      result = ReadCPUTopologyMetadata(data, HEADER_CPU_TOPOLOGY,
                                       size_without_header);
      break;
    case PERF_RECORD_HEADER_NUMA_TOPOLOGY:
      metadata_mask_ |= (1 << HEADER_NUMA_TOPOLOGY);
      result = ReadNUMATopologyMetadata(data, HEADER_NUMA_TOPOLOGY,
                                        size_without_header);
      break;
    default:
      LOG(WARNING) << "Event type " << header.type << " is not yet supported!";
      // Skip over the data in this event.
      data->SeekSet(data->Tell() + size_without_header);
      break;
    }
  }

  if (!result)
    return false;

  // The PERF_RECORD_HEADER_EVENT_TYPE events are obsolete, but if present
  // and PERF_RECORD_HEADER_EVENT_DESC metadata events are not, we should use
  // them. Otherwise, we should use prefer the _EVENT_DESC data.
  if (!(metadata_mask_ & (1 << HEADER_EVENT_DESC)) &&
      num_event_types == attrs_.size()) {
    // We can construct HEADER_EVENT_DESC:
    metadata_mask_ |= (1 << HEADER_EVENT_DESC);
  }
  return result;
}

bool PerfReader::WriteHeader(const struct perf_file_header& header,
                             DataWriter* data) const {
  CheckNoEventHeaderPadding();
  size_t size = sizeof(header);
  return data->WriteDataValue(&header, size, "file header");
}

bool PerfReader::WriteAttrs(const struct perf_file_header& header,
                            DataWriter* data) const {
  CheckNoPerfEventAttrPadding();
  const size_t id_offset = header.size;
  CHECK_EQ(id_offset, data->Tell());

  std::vector<struct perf_file_section> id_sections;
  id_sections.reserve(attrs_.size());
  for (size_t i = 0; i < attrs_.size(); i++) {
    const PerfFileAttr& attr = attrs_[i];
    size_t section_size =
        attr.ids.size() * sizeof(decltype(attr.ids)::value_type);
    id_sections.push_back(perf_file_section{data->Tell(), section_size});
    for (size_t j = 0; j < attr.ids.size(); j++) {
      const uint64_t id = attr.ids[j];
      if (!data->WriteDataValue(&id, sizeof(id), "ID info"))
        return false;
    }
  }

  CHECK_EQ(header.attrs.offset, data->Tell());
  for (size_t i = 0; i < attrs_.size(); i++) {
    const struct perf_file_section& id_section = id_sections[i];
    const PerfFileAttr& attr = attrs_[i];
    if (!data->WriteDataValue(&attr.attr, sizeof(attr.attr), "attribute") ||
        !data->WriteDataValue(&id_section, sizeof(id_section), "ID section")) {
      return false;
    }
  }
  return true;
}

bool PerfReader::WriteData(const struct perf_file_header& header,
                           DataWriter* data) const {
  CHECK_EQ(header.data.offset, data->Tell());
  for (size_t i = 0; i < events_.size(); ++i) {
    if (!data->WriteDataValue(events_[i].get(), events_[i]->header.size,
                              "event data")) {
      return false;
    }
  }
  return true;
}

bool PerfReader::WriteMetadata(const struct perf_file_header& header,
                               DataWriter* data) const {
  const size_t header_offset = header.data.offset + header.data.size;
  CHECK_EQ(header_offset, data->Tell());

  // There is one header for each feature pointing to the metadata for that
  // feature. If a feature has no metadata, the size field is zero.
  const size_t headers_size =
      GetNumSupportedMetadata() * sizeof(perf_file_section);
  const size_t metadata_offset = header_offset + headers_size;
  data->SeekSet(metadata_offset);

  // Record the new metadata offsets and sizes in this vector of info entries.
  std::vector<struct perf_file_section> metadata_sections;
  metadata_sections.reserve(GetNumSupportedMetadata());

  for (u32 type = HEADER_FIRST_FEATURE; type != HEADER_LAST_FEATURE; ++type) {
    if ((header.adds_features[0] & (1 << type)) == 0)
      continue;

    struct perf_file_section header_entry;
    header_entry.offset = data->Tell();
    // Write actual metadata to address metadata_offset
    switch (type) {
    case HEADER_TRACING_DATA:
      if (!data->WriteDataValue(tracing_data_.data(), tracing_data_.size(),
                                "tracing data")) {
        return false;
      }
      break;
    case HEADER_BUILD_ID:
      if (!WriteBuildIDMetadata(type, data))
        return false;
      break;
    case HEADER_HOSTNAME:
    case HEADER_OSRELEASE:
    case HEADER_VERSION:
    case HEADER_ARCH:
    case HEADER_CPUDESC:
    case HEADER_CPUID:
    case HEADER_CMDLINE:
      if (!WriteStringMetadata(type, data))
        return false;
      break;
    case HEADER_NRCPUS:
      if (!WriteUint32Metadata(type, data))
        return false;
      break;
    case HEADER_TOTAL_MEM:
      if (!WriteUint64Metadata(type, data))
        return false;
      break;
    case HEADER_EVENT_DESC:
      if (!WriteEventDescMetadata(type, data))
        return false;
      break;
    case HEADER_CPU_TOPOLOGY:
      if (!WriteCPUTopologyMetadata(type, data))
        return false;
      break;
    case HEADER_NUMA_TOPOLOGY:
      if (!WriteNUMATopologyMetadata(type, data))
        return false;
      break;
    case HEADER_BRANCH_STACK:
      break;
    default: LOG(ERROR) << "Unsupported metadata type: " << type;
      return false;
    }

    // Compute the size of the metadata that was just written. This is reflected
    // in how much the data write pointer has moved.
    header_entry.size = data->Tell() - header_entry.offset;
    metadata_sections.push_back(header_entry);
  }
  // Make sure we have recorded the right number of entries.
  CHECK_EQ(GetNumSupportedMetadata(), metadata_sections.size());

  // Now write the metadata offset and size info back to the metadata headers.
  size_t old_offset = data->Tell();
  data->SeekSet(header_offset);
  if (!data->WriteDataValue(metadata_sections.data(), headers_size,
                            "metadata section info")) {
    return false;
  }
  // Make sure the write pointer now points to the end of the metadata headers
  // and hence the beginning of the actual metadata.
  CHECK_EQ(metadata_offset, data->Tell());
  data->SeekSet(old_offset);

  return true;
}

bool PerfReader::WriteBuildIDMetadata(u32 type, DataWriter* data) const {
  CheckNoBuildIDEventPadding();
  for (size_t i = 0; i < build_id_events_.size(); ++i) {
    const build_id_event* event = build_id_events_[i].get();
    if (!data->WriteDataValue(event, event->header.size, "Build ID metadata"))
      return false;
  }
  return true;
}

bool PerfReader::WriteStringMetadata(u32 type, DataWriter* data) const {
  for (size_t i = 0; i < string_metadata_.size(); ++i) {
    const PerfStringMetadata& str_data = string_metadata_[i];
    if (str_data.type == type) {
      num_string_data_type num_strings = str_data.data.size();
      if (NeedsNumberOfStringData(type) &&
          !data->WriteDataValue(&num_strings, sizeof(num_strings),
                                "number of string metadata")) {
        return false;
      }

      for (size_t j = 0; j < num_strings; ++j) {
        const CStringWithLength& single_string = str_data.data[j];
        if (!WriteStringToBuffer(single_string, data))
          return false;
      }

      return true;
    }
  }
  LOG(ERROR) << "String metadata of type " << type << " not present";
  return false;
}

bool PerfReader::WriteUint32Metadata(u32 type, DataWriter* data) const {
  for (size_t i = 0; i < uint32_metadata_.size(); ++i) {
    const PerfUint32Metadata& uint32_data = uint32_metadata_[i];
    if (uint32_data.type == type) {
      const std::vector<uint32_t>& int_vector = uint32_data.data;

      for (size_t j = 0; j < int_vector.size(); ++j) {
        if (!data->WriteDataValue(&int_vector[j], sizeof(int_vector[j]),
                                  "uint32_t metadata")) {
          return false;
        }
      }

      return true;
    }
  }
  LOG(ERROR) << "Uint32 metadata of type " << type << " not present";
  return false;
}

bool PerfReader::WriteUint64Metadata(u32 type, DataWriter* data) const {
  for (size_t i = 0; i < uint64_metadata_.size(); ++i) {
    const PerfUint64Metadata& uint64_data = uint64_metadata_[i];
    if (uint64_data.type == type) {
      const std::vector<uint64_t>& int_vector = uint64_data.data;

      for (size_t j = 0; j < int_vector.size(); ++j) {
        if (!data->WriteDataValue(&int_vector[j], sizeof(int_vector[j]),
                                  "uint64_t metadata")) {
          return false;
        }
      }

      return true;
    }
  }
  LOG(ERROR) << "Uint64 metadata of type " << type << " not present";
  return false;
}

bool PerfReader::WriteEventDescMetadata(u32 type, DataWriter* data) const {
  CheckNoPerfEventAttrPadding();

  event_desc_num_events num_events = attrs_.size();
  if (!data->WriteDataValue(&num_events, sizeof(num_events),
                            "event_desc num_events")) {
    return false;
  }
  event_desc_attr_size attr_size = sizeof(perf_event_attr);
  if (!data->WriteDataValue(&attr_size, sizeof(attr_size),
                            "event_desc attr_size")) {
    return false;
  }

  for (size_t i = 0; i < num_events; ++i) {
    const PerfFileAttr& attr = attrs_[i];
    if (!data->WriteDataValue(&attr.attr, sizeof(attr.attr),
                              "event_desc attribute")) {
      return false;
    }

    event_desc_num_unique_ids num_ids = attr.ids.size();
    if (!data->WriteDataValue(&num_ids, sizeof(num_ids),
                              "event_desc num_unique_ids")) {
      return false;
    }

    CStringWithLength container;
    container.len = GetUint64AlignedStringLength(attr.name);
    container.str = attr.name;
    if (!WriteStringToBuffer(container, data))
      return false;

    if (!data->WriteDataValue(attr.ids.data(), num_ids * sizeof(attr.ids[0]),
                              "event_desc unique_ids")) {
      return false;
    }
  }
  return true;
}

bool PerfReader::WriteCPUTopologyMetadata(u32 type, DataWriter* data) const {
  const std::vector<CStringWithLength>& cores = cpu_topology_.core_siblings;
  num_siblings_type num_cores = cores.size();
  if (!data->WriteDataValue(&num_cores, sizeof(num_cores), "num cores"))
    return false;
  for (size_t i = 0; i < num_cores; ++i) {
    if (!WriteStringToBuffer(cores[i], data))
      return false;
  }

  const std::vector<CStringWithLength>& threads = cpu_topology_.thread_siblings;
  num_siblings_type num_threads = threads.size();
  if (!data->WriteDataValue(&num_threads, sizeof(num_threads), "num threads"))
    return false;
  for (size_t i = 0; i < num_threads; ++i) {
    if (!WriteStringToBuffer(threads[i], data))
      return false;
  }

  return true;
}

bool PerfReader::WriteNUMATopologyMetadata(u32 type, DataWriter* data) const {
  numa_topology_num_nodes_type num_nodes = numa_topology_.size();
  if (!data->WriteDataValue(&num_nodes, sizeof(num_nodes), "num nodes"))
    return false;

  for (size_t i = 0; i < num_nodes; ++i) {
    const PerfNodeTopologyMetadata& node = numa_topology_[i];
    if (!data->WriteDataValue(&node.id, sizeof(node.id), "node id") ||
        !data->WriteDataValue(&node.total_memory, sizeof(node.total_memory),
                              "node total memory") ||
        !data->WriteDataValue(&node.free_memory, sizeof(node.free_memory),
                              "node free memory") ||
        !WriteStringToBuffer(node.cpu_list, data)) {
    }
  }
  return true;
}

bool PerfReader::WriteEventTypes(const struct perf_file_header& header,
                                 DataWriter* data) const {
  CheckNoEventTypePadding();
  const size_t event_types_size = header.event_types.size;
  if (event_types_size == 0)
    return true;

  data->SeekSet(header.event_types.offset);
  for (size_t i = 0; i < attrs_.size(); ++i) {
    struct perf_trace_event_type event_type = {0};
    event_type.event_id = attrs_[i].attr.config;
    const string& name = attrs_[i].name;
    memcpy(&event_type.name, name.data(),
           std::min(name.size(), sizeof(event_type.name)));
    if (!data->WriteDataValue(&event_type, sizeof(event_type),
                              "event type info")) {
      return false;
    }
  }
  CHECK_EQ(event_types_size, data->Tell() - header.event_types.offset);
  return true;
}

bool PerfReader::ReadAttrEventBlock(DataReader* data, size_t size) {
  const size_t initial_offset = data->Tell();
  PerfFileAttr attr;
  if (!ReadEventAttr(data, &attr.attr))
    return false;

  // attr.attr.size has been upgraded to the current size of perf_event_attr.
  const size_t actual_attr_size = data->Tell() - initial_offset;

  const size_t num_ids =
      (size - actual_attr_size) / sizeof(decltype(attr.ids)::value_type);
  if (!ReadUniqueIDs(data, num_ids, &attr.ids))
    return false;

  // Event types are found many times in the perf data file.
  // Only add this event type if it is not already present.
  for (size_t i = 0; i < attrs_.size(); ++i) {
    if (attrs_[i].ids[0] == attr.ids[0])
      return true;
  }
  attrs_.push_back(attr);
  return true;
}

void PerfReader::MaybeSwapEventFields(event_t* event, bool is_cross_endian) {
  if (!is_cross_endian)
    return;
  uint32_t type = event->header.type;
  switch (type) {
  case PERF_RECORD_SAMPLE:
    break;
  case PERF_RECORD_MMAP:
    ByteSwap(&event->mmap.pid);
    ByteSwap(&event->mmap.tid);
    ByteSwap(&event->mmap.start);
    ByteSwap(&event->mmap.len);
    ByteSwap(&event->mmap.pgoff);
    break;
  case PERF_RECORD_MMAP2:
    ByteSwap(&event->mmap2.pid);
    ByteSwap(&event->mmap2.tid);
    ByteSwap(&event->mmap2.start);
    ByteSwap(&event->mmap2.len);
    ByteSwap(&event->mmap2.pgoff);
    ByteSwap(&event->mmap2.maj);
    ByteSwap(&event->mmap2.min);
    ByteSwap(&event->mmap2.ino);
    ByteSwap(&event->mmap2.ino_generation);
    ByteSwap(&event->mmap2.prot);
    ByteSwap(&event->mmap2.flags);
    break;
  case PERF_RECORD_FORK:
  case PERF_RECORD_EXIT:
    ByteSwap(&event->fork.pid);
    ByteSwap(&event->fork.tid);
    ByteSwap(&event->fork.ppid);
    ByteSwap(&event->fork.ptid);
    ByteSwap(&event->fork.time);
    break;
  case PERF_RECORD_COMM:
    ByteSwap(&event->comm.pid);
    ByteSwap(&event->comm.tid);
    break;
  case PERF_RECORD_LOST:
    ByteSwap(&event->lost.id);
    ByteSwap(&event->lost.lost);
    break;
  case PERF_RECORD_THROTTLE:
  case PERF_RECORD_UNTHROTTLE:
    ByteSwap(&event->throttle.time);
    ByteSwap(&event->throttle.id);
    ByteSwap(&event->throttle.stream_id);
    break;
  case PERF_RECORD_READ:
    ByteSwap(&event->read.pid);
    ByteSwap(&event->read.tid);
    ByteSwap(&event->read.value);
    ByteSwap(&event->read.time_enabled);
    ByteSwap(&event->read.time_running);
    ByteSwap(&event->read.id);
    break;
  default:
    LOG(FATAL) << "Unknown event type: " << type;
  }

  // ReadSampleInfo() will swap the additional perf_sample fields. Write them
  // back to |event| with WriteSampleInfo() so they are stored in native byte
  // order.
  CHECK_GT(attrs_.size(), 0U);
  SampleInfoReader reader(attrs_[0].attr, is_cross_endian);

  struct perf_sample sample_info;
  CHECK(reader.ReadPerfSampleInfo(*event, &sample_info))
      << "Error reading sample info from event.";
  CHECK(reader.WritePerfSampleInfo(sample_info, event))
      << "Error writing sample info back to event.";
}

size_t PerfReader::GetNumSupportedMetadata() const {
  return GetNumBits(metadata_mask_ & kSupportedMetadataMask);
}

size_t PerfReader::GetEventDescMetadataSize() const {
  size_t size = 0;
  if (metadata_mask_ & (1 << HEADER_EVENT_DESC)) {
    size += sizeof(event_desc_num_events) + sizeof(event_desc_attr_size);
    CStringWithLength dummy;
    for (size_t i = 0; i < attrs_.size(); ++i) {
      size += sizeof(perf_event_attr) + sizeof(dummy.len);
      size += sizeof(event_desc_num_unique_ids);
      size += GetUint64AlignedStringLength(attrs_[i].name);
      size += attrs_[i].ids.size() * sizeof(attrs_[i].ids[0]);
    }
  }
  return size;
}

size_t PerfReader::GetBuildIDMetadataSize() const {
  size_t size = 0;
  for (size_t i = 0; i < build_id_events_.size(); ++i)
    size += build_id_events_[i]->header.size;
  return size;
}

size_t PerfReader::GetStringMetadataSize() const {
  size_t size = 0;
  for (size_t i = 0; i < string_metadata_.size(); ++i) {
    const PerfStringMetadata& metadata = string_metadata_[i];
    if (NeedsNumberOfStringData(metadata.type))
      size += sizeof(num_string_data_type);

    for (size_t j = 0; j < metadata.data.size(); ++j) {
      const CStringWithLength& str = metadata.data[j];
      size += sizeof(str.len) + str.len;
    }
  }
  return size;
}

size_t PerfReader::GetUint32MetadataSize() const {
  size_t size = 0;
  for (size_t i = 0; i < uint32_metadata_.size(); ++i) {
    const PerfUint32Metadata& metadata = uint32_metadata_[i];
    size += metadata.data.size() * sizeof(metadata.data[0]);
  }
  return size;
}

size_t PerfReader::GetUint64MetadataSize() const {
  size_t size = 0;
  for (size_t i = 0; i < uint64_metadata_.size(); ++i) {
    const PerfUint64Metadata& metadata = uint64_metadata_[i];
    size += metadata.data.size() * sizeof(metadata.data[0]);
  }
  return size;
}

size_t PerfReader::GetCPUTopologyMetadataSize() const {
  // Core siblings.
  size_t size = sizeof(num_siblings_type);
  for (size_t i = 0; i < cpu_topology_.core_siblings.size(); ++i) {
    const CStringWithLength& str = cpu_topology_.core_siblings[i];
    size += sizeof(str.len) + str.len;
  }

  // Thread siblings.
  size += sizeof(num_siblings_type);
  for (size_t i = 0; i < cpu_topology_.thread_siblings.size(); ++i) {
    const CStringWithLength& str = cpu_topology_.thread_siblings[i];
    size += sizeof(str.len) + str.len;
  }

  return size;
}

size_t PerfReader::GetNUMATopologyMetadataSize() const {
  size_t size = sizeof(numa_topology_num_nodes_type);
  for (size_t i = 0; i < numa_topology_.size(); ++i) {
    const PerfNodeTopologyMetadata& node = numa_topology_[i];
    size += sizeof(node.id);
    size += sizeof(node.total_memory) + sizeof(node.free_memory);
    size += sizeof(node.cpu_list.len) + node.cpu_list.len;
  }
  return size;
}

bool PerfReader::NeedsNumberOfStringData(u32 type) const {
  return type == HEADER_CMDLINE;
}

bool PerfReader::LocalizeMMapFilenames(
    const std::map<string, string>& filename_map) {
  // Search for mmap/mmap2 events for which the filename needs to be updated.
  for (size_t i = 0; i < events_.size(); ++i) {
    string filename;
    size_t size_of_fixed_event_parts;
    event_t* event = events_[i].get();
    if (event->header.type == PERF_RECORD_MMAP) {
      filename = string(event->mmap.filename);
      size_of_fixed_event_parts =
          sizeof(event->mmap) - sizeof(event->mmap.filename);
    } else if (event->header.type == PERF_RECORD_MMAP2) {
      filename = string(event->mmap2.filename);
      size_of_fixed_event_parts =
          sizeof(event->mmap2) - sizeof(event->mmap2.filename);
    } else {
      continue;
    }

    const auto it = filename_map.find(filename);
    if (it == filename_map.end())  // not found
      continue;

    const string& new_filename = it->second;
    size_t old_len = GetUint64AlignedStringLength(filename);
    size_t new_len = GetUint64AlignedStringLength(new_filename);
    size_t old_offset = SampleInfoReader::GetPerfSampleDataOffset(*event);
    size_t sample_size = event->header.size - old_offset;

    int size_change = new_len - old_len;
    size_t new_size = event->header.size + size_change;
    size_t new_offset = old_offset + size_change;

    if (size_change > 0) {
      // Allocate memory for a new event.
      event_t* old_event = event;
      malloced_unique_ptr<event_t> new_event(CallocMemoryForEvent(new_size));

      // Copy over everything except filename and sample info.
      memcpy(new_event.get(), old_event, size_of_fixed_event_parts);

      // Copy over the sample info to the correct location.
      char* old_addr = reinterpret_cast<char*>(old_event);
      char* new_addr = reinterpret_cast<char*>(new_event.get());
      memcpy(new_addr + new_offset, old_addr + old_offset, sample_size);

      events_[i] = std::move(new_event);
      event = events_[i].get();
    } else if (size_change < 0) {
      // Move the perf sample data to its new location.
      // Since source and dest could overlap, use memmove instead of memcpy.
      char* start_addr = reinterpret_cast<char*>(event);
      memmove(start_addr + new_offset, start_addr + old_offset, sample_size);
    }

    // Copy over the new filename and fix the size of the event.
    char *event_filename = nullptr;
    if (event->header.type == PERF_RECORD_MMAP) {
      event_filename = event->mmap.filename;
    } else if (event->header.type == PERF_RECORD_MMAP2) {
      event_filename = event->mmap2.filename;
    } else {
      LOG(FATAL) << "Unexpected event type";  // Impossible
    }
    CHECK_GT(snprintf(event_filename, new_filename.size() + 1, "%s",
                      new_filename.c_str()),
             0);
    event->header.size = new_size;
  }

  return true;
}

}  // namespace quipper
