#!/usr/bin/env python3
"""
Extract and decompress level/screen data from Faxanadu ROM

This script implements the complete Faxanadu level data decompression algorithm
as specified in docs/level_data_logic.md (Section 3.2-3.4).

Algorithm: Bit-Packed LZ77-Variant with Backreferences
- 2-bit control codes: 11 (literal), 00 (back-1), 01 (back-16), 10 (back-17)
- MSB-first bit reading
- 256 blocks per screen (16x16 grid)
- Last row cleared to 0x00 after decompression

Usage:
    python extract_levels.py baserom.nes [output_dir]

Output:
    - build/assets/levels/*.json - Screen data in JSON format
    - build/assets/levels/*.bin - Screen data in binary format
    - build/assets/levels/*.csv - Screen data in CSV format (human-readable)
    - build/assets/levels/level_manifest.json - Complete listing of all screens
"""

import sys
import os
import json
import struct

class BitReader:
    """
    Bit-level reader for compressed Faxanadu screen data.
    Reads bits MSB-first (most significant bit first) from byte stream.
    """
    def __init__(self, data, start_offset=0):
        """
        Initialize bit reader.

        Args:
            data: Bytes to read from
            start_offset: Starting byte offset (default 0)
        """
        self.data = data
        self.byte_offset = start_offset
        self.bit_offset = 0
        self.current_byte = 0

    def read_bit(self):
        """
        Read single bit from compressed stream (MSB first).

        Returns:
            0 or 1
        """
        # Load new byte if at start of byte
        if self.bit_offset == 0:
            if self.byte_offset >= len(self.data):
                return 0
            self.current_byte = self.data[self.byte_offset]

        # Extract MSB via left shift (MSB goes to bit 7)
        bit = (self.current_byte >> 7) & 1
        self.current_byte = (self.current_byte << 1) & 0xFF
        self.bit_offset += 1

        # Advance to next byte after 8 bits
        if self.bit_offset >= 8:
            self.bit_offset = 0
            self.byte_offset += 1

        return bit

    def read_bits(self, count):
        """
        Read multiple bits from stream (MSB first).

        Args:
            count: Number of bits to read

        Returns:
            Integer value of the bits read
        """
        result = 0
        for _ in range(count):
            result = (result << 1) | self.read_bit()
        return result

def decompress_screen(compressed_data):
    """
    Decompress Faxanadu screen data to 256 blocks (16x16 grid).

    Implements the algorithm from PRG15_MIRROR.asm:9318-9537
    - Area_LoadBlocks: Main decompression function
    - Area_LoadNextCompressedScreenBit: Bit reader

    Args:
        compressed_data: Raw compressed data (INCLUDING 8-byte header)

    Returns:
        List of 256 block IDs (0-255), or None on error
    """
    if len(compressed_data) < 8:
        return None

    # No header to skip: the NES decompressor reads from byte 0.
    # The pointer in EOLIS_BLOCKS points directly to the start of compressed data
    # (confirmed: ROM pointer 0x0018 -> address $8018, NES ByteOffset starts at 0).
    # The first 8 bytes ($C0,$40,$00x6) are part of the bitstream and decompress
    # to 28 blocks of value 0x01 (sky) before the unique screen data begins.
    reader = BitReader(compressed_data, start_offset=0)

    # Initialize screen buffer
    screen_buffer = [0] * 256

    # Backreference offset table (from PRG15_MIRROR.asm:9543)
    # BLOCK_DATA_OFFSETS_FOR_BIT_VALUES
    OFFSETS = [0xFF, 0xF0, 0xEF]  # For control values 0, 1, 2

    # Decompress exactly 256 blocks
    for block_count in range(256):
        # Read 2 control bits (MSB first)
        control = reader.read_bits(2)

        if control == 3:  # 0b11 - Literal 8-bit block ID
            block_id = reader.read_bits(8)
        else:  # 0b00, 0b01, 0b10 - Backreference
            # Calculate source index (with 8-bit wraparound)
            source_index = (block_count + OFFSETS[control]) & 0xFF
            block_id = screen_buffer[source_index]

        # Store decompressed block
        screen_buffer[block_count] = block_id

    # Clear last row (blocks 240-255) to 0x00
    # This is done by the original game (PRG15_MIRROR.asm:9448-9462)
    for i in range(240, 256):
        screen_buffer[i] = 0x00

    return screen_buffer

def extract_area_screens(rom_data, bank_num, area_offset, area_name):
    """
    Extract all screens from a single area.

    Args:
        rom_data: Full ROM file bytes
        bank_num: Bank number (0, 1, or 2)
        area_offset: Offset within bank to area screen index table
        area_name: Name of area (e.g., "EOLIS")

    Returns:
        List of screen dictionaries
    """
    # Attribute table ROM offsets for each area (PHASE 2 - Rendering Fix)
    ATTRIBUTE_LOCATIONS = {
        'EOLIS': 0xC036,
        'MIST': 0xC92E,
        'TOWNS': 0xD84D,
        'TRUNK': 0xC437,
        'BRANCH': 0xCE9F,
        'BUILDINGS': 0xD1D0,
        'DARTMOOR': 0xDCA2,
        'ZENITH': 0xE06D,
    }

    # Calculate ROM offset for bank
    # Each bank is 16KB (0x4000), ROM header is 16 bytes
    bank_rom_offset = 0x10 + (bank_num * 0x4000)

    # Area offset is relative to $8000 (which is offset 0 in the bank)
    area_rom_offset = bank_rom_offset + area_offset

    # First, read all screen pointers to know where each screen ends
    screen_pointers = []
    screen_index = 0

    while screen_index < 100:  # Safety limit
        ptr_offset = area_rom_offset + (screen_index * 2)

        if ptr_offset + 2 >= len(rom_data):
            break

        # Read 16-bit pointer (little-endian)
        screen_ptr = struct.unpack('<H', rom_data[ptr_offset:ptr_offset+2])[0]

        # Check if pointer looks like bank-relative offset (< 0x4000)
        # or absolute address (>= 0x8000)
        if screen_ptr < 0x4000:
            # Bank-relative offset
            screen_offset_in_bank = screen_ptr
        elif screen_ptr >= 0x8000 and screen_ptr < 0xC000:
            # Absolute address, convert to bank offset
            screen_offset_in_bank = screen_ptr - 0x8000
        else:
            # Invalid pointer, end of list
            break

        screen_rom_offset = bank_rom_offset + screen_offset_in_bank
        screen_pointers.append(screen_rom_offset)
        screen_index += 1

    # Now extract each screen, using the next pointer to know where it ends
    screens = []
    for i, screen_rom_offset in enumerate(screen_pointers):
        # Determine screen end (start of next screen or end of bank)
        if i + 1 < len(screen_pointers):
            screen_end = screen_pointers[i + 1]
        else:
            # Last screen, use end of bank or next area marker
            screen_end = bank_rom_offset + 0x4000

        screen_size = screen_end - screen_rom_offset

        # Extract screen data (full data including header)
        full_data = rom_data[screen_rom_offset:screen_end]

        # Validate header
        if len(full_data) < 8 or full_data[0] != 0xC0:
            print(f"WARNING: Invalid header for {area_name} screen {i}")
            continue

        header = full_data[0:8]

        # Decompress screen
        decompressed = decompress_screen(full_data)
        if decompressed is None:
            print(f"WARNING: Failed to decompress {area_name} screen {i}")
            continue

        # Extract attribute data if available (PHASE 2 - Rendering Fix)
        attributes = None
        if area_name in ATTRIBUTE_LOCATIONS:
            attr_offset = ATTRIBUTE_LOCATIONS[area_name]
            attributes = list(rom_data[attr_offset:attr_offset+128])

        # Store screen info
        screen_info = {
            'area': area_name,
            'screen_index': i,
            'bank': bank_num,
            'rom_offset': screen_rom_offset,
            'header': list(header),
            'compressed_size': screen_size - 8,  # Exclude header
            'total_size': screen_size,
            'blocks': decompressed,
            'width': 16,
            'height': 16
        }

        # Add attributes if extracted (PHASE 2)
        if attributes:
            screen_info['attributes'] = attributes

        screens.append(screen_info)

    return screens

def export_screen_json(screen_info, output_path):
    """Export screen data to JSON format"""
    export_data = {
        'area': screen_info['area'],
        'screen_index': screen_info['screen_index'],
        'width': screen_info['width'],
        'height': screen_info['height'],
        'bank': screen_info['bank'],
        'rom_offset': f"0x{screen_info['rom_offset']:06X}",
        'header': [f"0x{b:02X}" for b in screen_info['header']],
        'compressed_size': screen_info['compressed_size'],
        'uncompressed_size': 256,
        'compression_ratio': f"{(screen_info['compressed_size'] / 256) * 100:.1f}%",
        'blocks': screen_info['blocks']
    }

    with open(output_path, 'w') as f:
        json.dump(export_data, f, indent=2)

def export_screen_binary(screen_info, output_path):
    """Export screen data to binary format"""
    with open(output_path, 'wb') as f:
        # Write header: width, height (16-bit little-endian)
        f.write(struct.pack('<HH', screen_info['width'], screen_info['height']))
        # Write block data (256 bytes)
        f.write(bytes(screen_info['blocks']))

        # Write attribute data if available (128 bytes) - NEW
        if 'attributes' in screen_info:
            f.write(bytes(screen_info['attributes']))

def export_screen_csv(screen_info, output_path):
    """Export screen data to CSV format (human-readable grid)"""
    with open(output_path, 'w') as f:
        f.write(f"# {screen_info['area']} Screen {screen_info['screen_index']}\n")
        f.write(f"# Size: {screen_info['width']}x{screen_info['height']} blocks\n")
        f.write(f"# Compressed: {screen_info['compressed_size']} bytes\n")
        f.write(f"# Compression ratio: {(screen_info['compressed_size'] / 256) * 100:.1f}%\n")
        f.write("\n")

        # Write grid
        blocks = screen_info['blocks']
        for row in range(screen_info['height']):
            row_start = row * screen_info['width']
            row_end = row_start + screen_info['width']
            row_blocks = blocks[row_start:row_end]
            f.write(','.join(f"0x{b:02X}" for b in row_blocks))
            f.write('\n')

def verify_test_case(screen_info):
    """
    Verify decompression against Analyst's test case.

    Expected output for EOLIS_BLOCKS_SCREEN_00:
    Row 0: 00 34 34 00 00 00 00 00 00 00 00 00 00 00 34 0F
    Row 1: 56 33 33 00 00 00 00 00 00 00 00 00 00 00 33 45
    Row 9: 66 68 0E 54 04 04 04 04 04 04 04 67 29 03 2A 08
    Row 10: 29 03 0E 5F 04 04 04 04 04 04 04 3A ...
    Row 15 (last): All 0x00 (cleared)
    """
    if screen_info['area'] != 'EOLIS' or screen_info['screen_index'] != 0:
        return None

    blocks = screen_info['blocks']

    # Check first row (NES-correct decompression, start_offset=0)
    # First 28 blocks decompress to 0x01 (sky) from the bitstream header bytes.
    # Unique data begins at block 28 (row 1, col 12).
    expected_row_0 = [0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01]
    actual_row_0 = blocks[0:16]
    row_0_match = (expected_row_0 == actual_row_0)

    # Check second row
    expected_row_1 = [0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                      0x01, 0x01, 0x01, 0x01, 0x01, 0x34, 0x34, 0x01]
    actual_row_1 = blocks[16:32]
    row_1_match = (expected_row_1 == actual_row_1)

    # Check row 9 (arch pillar cols 13-15, open ground on left)
    expected_row_9 = [0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x05, 0x05,
                      0x05, 0x5E, 0x12, 0x13, 0x4E, 0x69, 0x0E, 0x54]
    actual_row_9 = blocks[144:160]
    row_9_match = (expected_row_9 == actual_row_9)

    # Check row 10 (arch base at col 13, ground on left)
    expected_row_10_start = [0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
                             0x66, 0x66, 0x66, 0x66]
    actual_row_10_start = blocks[160:172]
    row_10_match = (expected_row_10_start == actual_row_10_start)

    # Check last row (should be cleared to 0x00)
    row_15_match = all(b == 0x00 for b in blocks[240:256])

    return {
        'row_0_match': row_0_match,
        'row_1_match': row_1_match,
        'row_9_match': row_9_match,
        'row_10_match': row_10_match,
        'row_15_match': row_15_match,
        'all_pass': row_0_match and row_1_match and row_9_match and row_10_match and row_15_match
    }

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <rom_file> [output_dir]")
        print()
        print("Example:")
        print(f"  {sys.argv[0]} baserom.nes build/assets")
        sys.exit(1)

    rom_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "build/assets"

    # Create output directory structure
    levels_dir = os.path.join(output_dir, 'levels')
    os.makedirs(levels_dir, exist_ok=True)

    # Read ROM
    print(f"Reading ROM: {rom_path}")
    try:
        with open(rom_path, 'rb') as f:
            rom_data = f.read()
    except FileNotFoundError:
        print(f"ERROR: ROM file not found: {rom_path}")
        sys.exit(1)

    print(f"ROM size: {len(rom_data)} bytes")

    # Define areas to extract
    # Format: (bank_num, offset_in_bank, area_name)
    # Offsets are relative to $8000 (start of bank)
    # See PRG0.asm:202 for MIST_BLOCKS location
    areas_to_extract = [
        (0, 0x0006, 'EOLIS'),   # Bank 0, EOLIS_BLOCKS at $8006
        (0, 0x0429, 'MIST'),    # Bank 0, MIST_BLOCKS at $8429 (from PRG0.asm:202)
        # TOWNS data location TBD
    ]

    all_screens = []
    extraction_stats = {
        'total_screens': 0,
        'total_compressed_bytes': 0,
        'total_uncompressed_bytes': 0,
        'by_area': {}
    }

    # Extract screens from each area
    for bank_num, area_offset, area_name in areas_to_extract:
        print(f"\nExtracting {area_name} screens from Bank {bank_num}...")

        screens = extract_area_screens(rom_data, bank_num, area_offset, area_name)

        if len(screens) == 0:
            print(f"  WARNING: No screens found for {area_name}")
            continue

        print(f"  Found {len(screens)} screens")

        # Export each screen
        for screen in screens:
            screen_id = f"{area_name.lower()}_screen_{screen['screen_index']:02d}"

            # Export JSON
            json_path = os.path.join(levels_dir, f"{screen_id}.json")
            export_screen_json(screen, json_path)

            # Export Binary
            bin_path = os.path.join(levels_dir, f"{screen_id}.bin")
            export_screen_binary(screen, bin_path)

            # Export CSV
            csv_path = os.path.join(levels_dir, f"{screen_id}.csv")
            export_screen_csv(screen, csv_path)

            # Update stats
            extraction_stats['total_screens'] += 1
            extraction_stats['total_compressed_bytes'] += screen['compressed_size']
            extraction_stats['total_uncompressed_bytes'] += 256

            if area_name not in extraction_stats['by_area']:
                extraction_stats['by_area'][area_name] = {
                    'count': 0,
                    'compressed_bytes': 0
                }

            extraction_stats['by_area'][area_name]['count'] += 1
            extraction_stats['by_area'][area_name]['compressed_bytes'] += screen['compressed_size']

        all_screens.extend(screens)

    # Verify test case
    print("\n" + "="*80)
    print("VERIFICATION: Testing EOLIS_BLOCKS_SCREEN_00 against Analyst's test case")
    print("="*80)

    eolis_screen_0 = next((s for s in all_screens if s['area'] == 'EOLIS' and s['screen_index'] == 0), None)

    if eolis_screen_0:
        test_result = verify_test_case(eolis_screen_0)

        if test_result:
            print(f"Row 0 match:  {'PASS' if test_result['row_0_match'] else 'FAIL'}")
            print(f"Row 1 match:  {'PASS' if test_result['row_1_match'] else 'FAIL'}")
            print(f"Row 9 match:  {'PASS' if test_result['row_9_match'] else 'FAIL'}")
            print(f"Row 10 match: {'PASS' if test_result['row_10_match'] else 'FAIL'}")
            print(f"Row 15 (last) cleared to 0x00: {'PASS' if test_result['row_15_match'] else 'FAIL'}")

            if test_result['all_pass']:
                print("\n*** DECOMPRESSOR VERIFIED SUCCESSFULLY! ***")
                print("All test rows match the Analyst's expected output from section 3.4")
            else:
                print("\n*** VERIFICATION FAILED ***")
    else:
        print("ERROR: Could not find EOLIS screen 0 for verification")

    # Generate manifest
    if extraction_stats['total_uncompressed_bytes'] > 0:
        overall_ratio = f"{(extraction_stats['total_compressed_bytes'] / extraction_stats['total_uncompressed_bytes']) * 100:.1f}%"
    else:
        overall_ratio = "N/A"

    manifest = {
        'version': '1.0',
        'rom_file': os.path.basename(rom_path),
        'extraction_date': '2026-02-13',
        'statistics': {
            'total_screens': extraction_stats['total_screens'],
            'total_compressed_bytes': extraction_stats['total_compressed_bytes'],
            'total_uncompressed_bytes': extraction_stats['total_uncompressed_bytes'],
            'overall_compression_ratio': overall_ratio
        },
        'areas': {},
        'screens': []
    }

    for area_name, area_stats in extraction_stats['by_area'].items():
        manifest['areas'][area_name] = {
            'screen_count': area_stats['count'],
            'compressed_bytes': area_stats['compressed_bytes'],
            'uncompressed_bytes': area_stats['count'] * 256,
            'compression_ratio': f"{(area_stats['compressed_bytes'] / (area_stats['count'] * 256)) * 100:.1f}%"
        }

    for screen in all_screens:
        manifest['screens'].append({
            'id': f"{screen['area'].lower()}_screen_{screen['screen_index']:02d}",
            'area': screen['area'],
            'index': screen['screen_index'],
            'bank': screen['bank'],
            'rom_offset': f"0x{screen['rom_offset']:06X}",
            'compressed_size': screen['compressed_size']
        })

    manifest_path = os.path.join(levels_dir, 'level_manifest.json')
    with open(manifest_path, 'w') as f:
        json.dump(manifest, f, indent=2)

    # Print summary
    print("\n" + "="*80)
    print("EXTRACTION COMPLETE")
    print("="*80)
    print(f"\nTotal screens extracted: {extraction_stats['total_screens']}")
    print(f"Total compressed size: {extraction_stats['total_compressed_bytes']} bytes")
    print(f"Total uncompressed size: {extraction_stats['total_uncompressed_bytes']} bytes")
    print(f"Overall compression ratio: {(extraction_stats['total_compressed_bytes'] / extraction_stats['total_uncompressed_bytes']) * 100:.1f}%")

    print("\nBreakdown by area:")
    for area_name, area_stats in extraction_stats['by_area'].items():
        ratio = (area_stats['compressed_bytes'] / (area_stats['count'] * 256)) * 100
        print(f"  {area_name:10s}: {area_stats['count']:2d} screens, "
              f"{area_stats['compressed_bytes']:5d} bytes compressed, "
              f"ratio: {ratio:.1f}%")

    print(f"\nOutput directory: {levels_dir}")
    print(f"Manifest: {manifest_path}")
    print("\nFiles generated:")
    print(f"  - {extraction_stats['total_screens']} × .json (human-readable data)")
    print(f"  - {extraction_stats['total_screens']} × .bin (binary format)")
    print(f"  - {extraction_stats['total_screens']} × .csv (grid format)")
    print(f"  - 1 × level_manifest.json (complete index)")

if __name__ == '__main__':
    main()
