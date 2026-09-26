"""MMC2/MMC4 post-read latch contracts through the CPU's $2007 port."""
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract


def latch_fixtures():
    for mapper in (9,):
        yield handoff(f'latch_mapper{mapper}_prg', mapper, [(0xa000, 3)],
                      3 if mapper == 9 else 6, chr_kb=128)
        operations = [
            ('cpu', 0xb000, 1), ('cpu', 0xc000, 2),
            ('cpu', 0xd000, 3), ('cpu', 0xe000, 4),
            ('read', 0x0fe8, 11),  # register writes updated the active FE bank
            ('read', 0, 8),
            ('write', 0x0fd8, 0), ('read', 0, 8),  # /WR cannot switch
            ('read', 0x0fd9, 11),
            ('read', 0, 8 if mapper == 9 else 4),
            ('read', 0x0fd8, 11 if mapper == 9 else 7),
            ('read', 0, 4),
            ('read', 0x0fe8, 7), ('read', 0, 8),
            ('read', 0x1fe8, 19), ('read', 0x1000, 16),
            ('read', 0x1fdf, 19), ('read', 0x1000, 12),
            ('read', 0x1fef, 15), ('read', 0x1000, 16),
            ('cpu', 0xf000, 0), ('write', 0x2000, 0x71), ('read', 0x2800, 0x71),
            ('cpu', 0xf000, 1), ('read', 0x2400, 0x71),
            # Rendering also traverses both trigger tiles, on every tile row.
            *[('write', 0x2000 + i, 0xfd + i % 2) for i in range(16)],
        ]
        if mapper == 10:
            operations += [('cpu', 0x6000, 0x79), ('cpu_read', 0x6000, 0x79)]
        name, image, seeds, expected = ppu_contract(mapper, 128, 128, operations)
        yield 'latch_' + name, image, seeds, expected
