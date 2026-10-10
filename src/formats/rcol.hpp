#pragma once

#include "binreader.hpp"
#include "dbpf.hpp"

#include <span>
#include <string>
#include <vector>

namespace os2 {

// RCOL ("resource collection") container used by the scenegraph types:
// CRES, SHPE, GMND, GMDC, TXTR, LIFO, TXMT and the light resources.
//
//   [u32 0xFFFF0001]?            version flag; links then carry instanceHi
//   u32 linkCount, links         {group, instance, [instanceHi], type}
//   u32 blockCount, u32 typeId[blockCount]
//   blocks: str7 className, u32 typeId, body (starts with u32 version)
//
// Blocks do not store their size, so the body of each block runs until the
// header of the next one (located by its className + typeId) or the end.
class Rcol {
public:
    struct Block {
        std::string className;
        uint32_t typeId = 0;
        std::span<const uint8_t> body; // starts at the block's u32 version
    };

    explicit Rcol(std::span<const uint8_t> data);

    bool hasInstanceHi = false;
    std::vector<Tgi> links;
    std::vector<Block> blocks;

    // First block with the given class name, or nullptr.
    const Block* find(std::string_view className) const;
};

// Cheap check that a resource looks like an RCOL.
bool rcolLooksValid(std::span<const uint8_t> data);

// Reads the "str7 className, u32 blockId" header of a block embedded in another
// block's body and checks the class name.
void readEmbeddedHeader(BinReader& r, std::string_view expected);

// cSGResource: the resource's own name ("..._shpe", "..._txtr").
struct SgResource {
    uint32_t version = 0;
    std::string fileName;
    static SgResource read(BinReader& r); // reads the embedded header too
};

// cObjectGraphNode: extensions (indices of other blocks) attached to a node.
struct ObjectGraphNode {
    struct Extension {
        bool enabled = false;
        bool dependent = false;
        uint32_t index = 0;
    };
    uint32_t version = 0;
    std::vector<Extension> extensions;
    std::string fileName; // version 4 only
    static ObjectGraphNode read(BinReader& r);
};

// cReferentNode: wrapper around a cObjectGraphNode.
struct ReferentNode {
    uint32_t version = 0;
    ObjectGraphNode graph;
    static ReferentNode read(BinReader& r);
};

// cCompositionTreeNode: wrapper around a cObjectGraphNode.
struct CompositionTreeNode {
    uint32_t version = 0;
    ObjectGraphNode graph;
    static CompositionTreeNode read(BinReader& r);
};

} // namespace os2
