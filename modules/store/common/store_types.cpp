#include "store/common/store_types.h"

namespace raftdemo
{
    bool ChunkLocation::IsValid() const
    {
        return !node_id.empty() && !chunk_id.empty();
    }

} // namespace raftdemo
