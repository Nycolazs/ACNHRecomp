// NookKit JSON patches (nookkit.cpp): files in a mod's romfs/NookKit/{Message,Bcsv,Pack}/ that change game messages,
// BCSV table rows and files in .pack archives, applied to the game's files in memory when the RomFS mounts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nx {

// The enabled mods' romfs folders, the first listed (the one that wins) first (hle_fs.cpp).
std::vector<std::string> romfs_mod_roots();

// Reads the patches of every enabled mod and serves the files they change, patched, as virtual files (with the
// ResourceSizeTable raised to fit them). Once, when the RomFS mounts; nothing when no mod has patches.
void nookkit_install();

namespace nookkit {

// ---- NookKit/Message: MSBT messages ------------------------------------------------------------------------------

struct Message {
    std::map<std::string, std::u16string> text;  // locale ("USen", ..., "default") -> MsgStd UTF-16, NUL included
    enum class Attr { None, Like, Bytes } attr = Attr::None;
    std::string like;   // Attr::Like: the label whose ATR1 entry it copies
    std::string bytes;  // Attr::Bytes: the ATR1 entry itself
    std::string source;  // the JSON file it came from
    const std::u16string* text_for(const std::string& locale) const;
};

struct FilePatch {
    std::string archive;  // "LayoutMsg", ...; empty when the patches did not say
    std::map<std::string, Message> labels;
};

// Every loaded patch by msbt name (without ".msbt"). A label patched twice keeps the later one.
struct Patches {
    std::map<std::string, FilePatch> files;
    // Adds the patches of one JSON file (one patch or an array of them); nothing when any of them is wrong.
    bool add(const std::string& source, const std::string& json, std::string* error);
    size_t count() const;
};

// A Message/<Archive>_<locale>.sarc.zs with the messages of `files` (msbt name -> patch) replaced or added. Empty
// when none of its files is patched or the archive cannot be read; *messages counts the changed messages, *old_size
// and *new_size are the archive's decompressed sizes.
std::string patch_archive(const std::string& sarc_zs, const std::string& locale,
                          const std::map<std::string, const FilePatch*>& files, int* messages, size_t* old_size,
                          size_t* new_size);

// The msbt files (names as the SARC has them) of a Message archive.
std::vector<std::string> archive_files(const std::string& sarc_zs);

// ---- NookKit/Bcsv: table rows ------------------------------------------------------------------------------------

struct BcsvCell {
    std::string key;    // the column as the patch names it
    uint32_t hash = 0;  // the column's hash: CRC32 of the name, or the name's own "0x%08X"
    enum Kind { Unsigned, Signed, Float, Hash, Text, Bytes } kind = Unsigned;
    uint32_t value = 0;  // Unsigned, Signed (two's complement), Float (bits), Hash
    std::string bytes;   // Text, Bytes
};

struct BcsvSource {
    std::string name;  // the JSON file
    std::vector<std::vector<BcsvCell>> rows;
};

// Every loaded patch by table ("ItemParam" for Bcsv/ItemParam.bcsv), in load order: a row patched twice keeps the
// later one. A row replaces the table's row with the same key (the first of the known key columns the table has) and
// is added after the others otherwise.
struct BcsvPatches {
    std::map<std::string, std::vector<BcsvSource>> files;
    bool add(const std::string& source, const std::string& json, std::string* error);
    // The table with the patches of `file`; empty when none applies. A patch with a cell that does not fit the table
    // is skipped whole (logged).
    std::string apply(const std::string& file, const std::string& table, int* replaced, int* added) const;
};

// ---- NookKit/Pack: BYML files in Pack/<archive>.pack ----------------------------------------------------------------

struct BymlNode {
    enum Kind { String, Array, Dict, Bool, Int, Float, Int64, Null } kind = Null;
    std::string str;
    std::vector<BymlNode> arr;
    std::map<std::string, BymlNode> dict;  // sorted, as the game looks keys up by binary search
    int64_t num = 0;                        // Bool, Int, Int64
    float f = 0;
};

struct PackFile {
    std::string source;
    BymlNode root;  // the whole file
};

// Every loaded patch by archive ("StaticParam") then file in it ("Param/NpcHouse/wol13.byml"); later ones win.
struct PackPatches {
    std::map<std::string, std::map<std::string, PackFile>> archives;
    bool add(const std::string& source, const std::string& json, std::string* error);
    // The .pack (a SARC) with its patched files replaced, each written in the original's byte order and version;
    // empty when none applies. A patched file the archive does not have is logged (JSON only replaces files).
    std::string apply(const std::string& archive, const std::string& sarc, int* files) const;
};

// A BYML document (version 1 to 4, "YB" little or "BY" big endian); empty when the root is not a container.
std::string write_byml(const BymlNode& root, bool big_endian, uint16_t version);

// ---- shared -------------------------------------------------------------------------------------------------------

// A ResourceSizeTable (Yaz0 or plain) with the entries named in `growth` ("Message/LayoutMsg_USen.sarc") raised by
// their byte counts. Empty when it cannot be read.
std::string raise_rstb(const std::string& table, const std::map<std::string, int64_t>& growth);

// For tests/nookkit_test.cpp: the msbt files of an archive that do not rebuild to the same bytes (-1: the archive
// cannot be read), one message's text (UTF-16, NUL included; empty when not there), and a file of a SARC.
int roundtrip_failures(const std::string& sarc_zs);
std::u16string message_text(const std::string& sarc_zs, const std::string& file, const std::string& label);
std::string sarc_file(const std::string& sarc, const std::string& name);

}  // namespace nookkit
}  // namespace nx
