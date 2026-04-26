#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace raftdemo {

struct SnapshotMeta {
  // Path of the state-machine snapshot data file. In the new directory layout
  // this points to snapshot_<index>/data.bin.
  std::string snapshot_path;

  // Path of the snapshot metadata file. In the new directory layout this points
  // to snapshot_<index>/__raft_snapshot_meta.
  std::string meta_path;

  // Snapshot directory path, for example snapshot_00000000000000000120.
  // Older flat-file snapshots leave this empty.
  std::string snapshot_dir;

  std::uint64_t last_included_index{0};
  std::uint64_t last_included_term{0};
  std::uint64_t created_unix_ms{0};

  // Checksum of snapshot_path. A zero value is allowed only for legacy flat
  // snapshots that did not record checksums.
  std::uint32_t data_checksum{0};
};

class ISnapshotStorage {
 public:
  virtual ~ISnapshotStorage() = default;

  virtual bool SaveSnapshotFile(const std::string& input_snapshot_file,
                                std::uint64_t last_included_index,
                                std::uint64_t last_included_term,
                                SnapshotMeta* out_meta,
                                std::string* error) = 0;

  // List valid snapshots, newest/highest-index first. Corrupted snapshots are ignored.
  virtual bool ListSnapshots(std::vector<SnapshotMeta>* out,
                             std::string* error) = 0;

  virtual bool LoadLatestValidSnapshot(SnapshotMeta* out_meta,
                                       bool* has_snapshot,
                                       std::string* error) = 0;

  virtual bool PruneSnapshots(std::size_t max_keep,
                              std::string* error) = 0;

  virtual const std::string& SnapshotDir() const = 0;
};

std::unique_ptr<ISnapshotStorage> CreateFileSnapshotStorage(std::string snapshot_dir,
                                                            std::string file_prefix);

}  // namespace raftdemo
