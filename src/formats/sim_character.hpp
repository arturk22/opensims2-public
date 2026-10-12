#pragma once

#include "sim.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace os2 {

class DbpfPackage;

// What a neighbourhood character file (Neighborhoods/N###/Characters/
// N###_User#####.package) says about how a Sim looks. Its AGED property set
// (0xAC598EAC, instance 1) and the 3IDR of the same instance hold:
//   age, gender (bits as in the catalog), skincolor ("0000000N-..." = tone sN),
//   skeletonkeyidx (3IDR index of the skeleton CRES: auskel, cuskel, ...),
//   listcnt and per entry lk<i> (category bits << 16 | part bits: 1 hair,
//   2 face, 4 top, 8 body, 16 bottom, ...) and le<i>_0 (3IDR index of its
//   GZPS). Category 1 is everyday wear (inferred from the N001 Sims). Catalog
//   parts are referenced by TGI; the face is a GZPS of the character file
//   whose CRES (#0x7F861BC1!age<N>_0_cres) is the Sim's own baked face mesh,
//   with a TXMT listing the textures to composite (baseTexture0..N-1:
//   face skin, eyes, beard, brows, ...).
struct Character {
    std::filesystem::path file;
    uint32_t age = 8, gender = 2;
    int skinTone = 1;
    bool skinToneSaved = false; // skincolor named a tone (1-4, 5 the alien), not the default
    std::string skeleton = "auskel";               // CRES name without "_cres"
    std::map<uint32_t, std::vector<Tgi>> outfits;  // lk key -> GZPS TGIs
    std::vector<Tgi> references;

    // The parts of one category (1 = everyday), by part bits.
    std::map<uint32_t, Tgi> outfit(uint32_t category = 1) const;
    // What the Sim wears in outfit category `category` (bits as the keys':
    // 0x001 everyday, 0x008 swimwear, 0x010 pyjamas, 0x020 formal, 0x040
    // underwear, 0x080 naked, 0x200 athletic, ...): that category's parts,
    // with the hair and face of everyday wear when it has none of its own
    // (the "naked" lists name only hair, top and bottom); everyday wear when
    // the Sim has no such outfit.
    std::map<uint32_t, Tgi> wear(uint32_t category) const;
    // The face list's other entries in that category (or everyday wear):
    // the mesh overlays (glasses, beards with a mesh) as their own GZPS.
    std::vector<Tgi> faceExtras(uint32_t category) const;
    // Prefix of the catalog names: "am", "af", "em", "cu", "pu", ...
    std::string prefix(bool unisexChildren = false) const;
};

Character readCharacter(DbpfPackage& package, ResourceIndex& index);

// The face skin texture name a character file's face material gives
// ("stdMatBaseTextureName"/"baseTexture0"), migrated for files our CAS saved
// before its face fixes: a "<p>face-s<n>" the game does not have (round 12),
// and a face of another tone than the body's (`skinTone`, 0 = unknown): the
// stray "afface-s3" CASFace.package's afface_s1_txmt names (round 16).
// Anything else is returned as it is.
std::string migrateFaceSkin(ResourceIndex& index, const std::string& texture, int skinTone = 0);

// Assembles a Sim from its character file over the game's resources `index`:
// the outfit of `category` (Character::wear; with the skin tone under SimSkin
// clothes), its own face mesh and face textures (the base and up to three
// overlays), on its skeleton. Parts that cannot be found are reported in the
// model's warnings.
SkinnedModel loadCharacterSim(ResourceIndex& index, const std::vector<SimPart>& parts,
                              const std::filesystem::path& characterFile, uint32_t category = 1);

} // namespace os2
