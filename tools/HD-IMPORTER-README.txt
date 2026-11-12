Mesen HD Pack Importer for NESRecomp

Requires Python 3.11 or later. Extract the entire tool ZIP.
This shared importer supports any NESRecomp game using the HD-pack Mods runtime.
Supply the target game's game ID, your original ROM, and the pack's identity,
author and license. Game launchers or title wrappers can supply that metadata.

python tools/package_hdpack.py --pack "path/to/download.zip" --rom "path/to/original.nes" --game-id "the-game-id" --id "creator.pack-id" --name "Pack display name" --author "Pack creator" --license "Original pack terms" --out "My-HD-Pack.nesmod"

--pack accepts a folder containing hires.txt, a ZIP containing the pack (including
a top-level directory), or a wrapper ZIP/folder containing an inner pack ZIP.
Pack discovery uses hires.txt and does not depend on any game or ZIP filename.
If a download contains multiple packs, select the intended inner ZIP or folder.
Wrapper text/license/credit files are preserved automatically. Extra credits can
be supplied with --notice "path/to/credits.txt" (repeat for additional files).

Install the resulting .nesmod in the target game's Mods page and enable its HD
feature before Play. The pack is disabled by default. Your ROM stays unchanged;
an optional pack IPS is verified against that ROM and applied in memory at boot.
Write the generated .nesmod outside the selected hires.txt folder. ROM images
cannot be included in packages or as notice files.

Nested input is limited to 256 MiB, 4096 entries and three ZIP levels; directories
are limited to eight levels. Links, traversal and ambiguous paths are rejected.
Temporary extraction is removed after success or failure.

This is an importer, not an implementation of every Mesen feature. It preserves
pack assets. Rendering follows the game's supported HD sampler; replacement
music and sound effects remain unsupported in the current NESRecomp runtime.
No ROM, graphics, music or sound assets are included in this tool download.
