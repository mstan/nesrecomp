"""Generic Mesen HD import regressions; synthetic inputs, no game launches."""
import argparse
import hashlib
import io
from pathlib import Path
import stat
import tempfile
import tomllib
import unittest
import zipfile
import zlib
import package_hdpack as importer


def zip_bytes(files):
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    return output.getvalue()


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.image = b'NES\x1a' + bytes([1, 0]) + bytes(10) + bytes(16384)
        self.rom = self.root / 'original.nes'
        self.rom.write_bytes(self.image)
        self.files = {'hires.txt': b'<ver>106\n<scale>2\n<img>art.png\n',
                      'art.png': b'synthetic pixels', 'LICENSE.txt': b'creator terms'}

    def convert(self, source, name='out.nesmod', **changes):
        args = argparse.Namespace(id='another-game.hd', version='1.0.0', name='Another game HD',
            game_id='another-game-us', author='A pack creator', license='Creator terms',
            rom=self.rom, out=self.root/name, notice=[])
        vars(args).update(changes)
        with importer.pack_folder(source) as (folder, notices):
            importer.package(args, folder, notices)
            extracted = folder
        if source.is_file():
            self.assertFalse(extracted.exists(), 'temporary pack leaked')
        self.assertEqual(self.rom.read_bytes(), self.image)
        with zipfile.ZipFile(args.out) as archive:
            content = {n: archive.read(n) for n in archive.namelist()}
        manifest = tomllib.loads(content.pop('manifest.toml').decode())
        self.assertEqual(manifest['target'][0]['game_id'], 'another-game-us')
        self.assertEqual(manifest['author'], 'A pack creator')
        self.assertFalse(manifest['feature'][0]['default_enabled'])
        return manifest, content

    def test_folder_zip_subfolder_and_arbitrarily_named_nested_zip(self):
        folder = self.root/'source'; folder.mkdir()
        for name, data in self.files.items(): (folder/name).write_bytes(data)
        direct = self.root/'TotallyDifferentTitle.zip'; direct.write_bytes(zip_bytes(self.files))
        subfolder = self.root/'WithDirectory.zip'
        subfolder.write_bytes(zip_bytes({'Release/TexturePack/'+n: d for n,d in self.files.items()}))
        wrapper = self.root/'DownloadFirst.zip'
        wrapper.write_bytes(zip_bytes({'AnyNameHere.zip':direct.read_bytes(), 'Readme.txt':b'outer credits'}))
        for i, source in enumerate((folder,direct,subfolder,wrapper)):
            with self.subTest(source=source):
                _, content = self.convert(source, f'out{i}.nesmod')
                for name, data in self.files.items(): self.assertEqual(content['pack/'+name],data)
                if source==wrapper:
                    self.assertIn(b'outer credits',[d for n,d in content.items() if n.startswith('notices/')])

    def test_ips_overlay_keeps_original_input(self):
        patch = b'PATCH'+bytes([0,0,16,0,1,0xA9])+b'EOF'
        files = dict(self.files, **{'fix.ips':patch})
        files['hires.txt'] += f'<patch>fix.ips,{hashlib.sha1(self.image).hexdigest()}\n'.encode()
        source=self.root/'AnyTitle.zip';source.write_bytes(zip_bytes(files))
        manifest, content = self.convert(source,'overlay.nesmod')
        self.assertEqual(manifest['hd_pack'][0]['patch'], 'pack/fix.ips')
        staged = self.image[:16] + bytes([0xA9]) + self.image[17:]
        self.assertEqual(manifest['hd_pack'][0]['patched_rom_crc32'], f'{zlib.crc32(staged[16:]):08x}')
        self.assertEqual(content['pack/fix.ips'], patch)
        self.assertEqual(self.rom.read_bytes(), self.image)

    def test_ips_invalid_records_leave_no_archive(self):
        for index, patch in enumerate((b'PATCH'+bytes([0,0,0,0,1,0xA9])+b'EOF',
                                       b'PATCH'+bytes([0,0,16,0,0,0,3])+b'EOF',
                                       b'PATCHEOF'+bytes([0,0,1]))):
            files = dict(self.files, **{'fix.ips':patch})
            files['hires.txt'] += f'<patch>fix.ips,{hashlib.sha1(self.image).hexdigest()}\n'.encode()
            source=self.root/f'invalid-{index}.zip';source.write_bytes(zip_bytes(files))
            name=f'invalid-{index}.nesmod'
            with self.assertRaises(ValueError):self.convert(source,name)
            self.assertFalse((self.root/name).exists())
        self.assertEqual(self.rom.read_bytes(), self.image)

    def test_ips_rle_and_original_identity(self):
        patch = b'PATCH' + bytes([0, 0, 16, 0, 0, 0, 3, 0xA9]) + b'EOF'
        staged = importer.overlay_image(self.image, patch)
        self.assertEqual(staged[16:19], bytes([0xA9]) * 3)
        self.assertEqual(staged[19:], self.image[19:])
        self.assertEqual(importer.overlay_image(self.image, patch + len(self.image).to_bytes(3, 'big')), staged)
        files = dict(self.files, **{'fix.ips': patch})
        files['hires.txt'] += b'<patch>fix.ips,' + b'0' * 40 + b'\n'
        source = self.root / 'wrong-original.zip'
        source.write_bytes(zip_bytes(files))
        with self.assertRaisesRegex(ValueError, 'different original ROM revision'):
            self.convert(source, 'wrong-original.nesmod')
        self.assertFalse((self.root / 'wrong-original.nesmod').exists())
        self.assertEqual(self.rom.read_bytes(), self.image)

    def test_multiple_packs_are_ambiguous(self):
        source=self.root/'options.zip'
        source.write_bytes(zip_bytes({'option-one.zip':zip_bytes(self.files),'option-two.zip':zip_bytes(self.files)}))
        with self.assertRaisesRegex(ValueError,'Multiple HD packs'),importer.pack_folder(source):
            self.fail('guessed between two packs')

    def test_unsafe_names_links_and_case_collisions_rejected(self):
        for name in ('../escaped','/absolute','C:/absolute','dir\\..\\escaped','trailing. ','HIRES.TXT','NUL.png','CON'):
            source=self.root/'unsafe.zip';source.write_bytes(zip_bytes({**self.files,name:b'bad'}))
            with self.subTest(name=name),self.assertRaises(ValueError),importer.pack_folder(source):
                self.fail('unsafe archive accepted')
        source=self.root/'link.zip'
        with zipfile.ZipFile(source,'w') as archive:
            archive.writestr('hires.txt',self.files['hires.txt'])
            info=zipfile.ZipInfo('link');info.external_attr=(stat.S_IFLNK|0o777)<<16
            archive.writestr(info,'../target')
        with self.assertRaises(ValueError),importer.pack_folder(source): self.fail('link accepted')
        self.assertFalse((self.root.parent/'escaped').exists())

    def test_limits_missing_pack_and_filesystem_root(self):
        source=self.root/'pack.zip';source.write_bytes(zip_bytes(self.files))
        previous=importer.MAX_BYTES
        try:
            importer.MAX_BYTES=1
            with self.assertRaisesRegex(ValueError,'limit'),importer.pack_folder(source): self.fail('limit ignored')
        finally: importer.MAX_BYTES=previous
        source.write_bytes(zip_bytes({'docs.zip':zip_bytes({'readme.txt':b'no textures'})}))
        with self.assertRaisesRegex(ValueError,'No Mesen'),importer.pack_folder(source): self.fail('missing pack accepted')
        with self.assertRaisesRegex(ValueError,'filesystem root'),importer.pack_folder(Path(self.root.anchor)):
            self.fail('filesystem root scanned')
        nested=zip_bytes(self.files)
        for _ in range(3): nested=zip_bytes({'next.zip':nested})
        source.write_bytes(nested)
        with self.assertRaisesRegex(ValueError,'nested ZIP depth'),importer.pack_folder(source): self.fail('depth limit ignored')

    def test_roms_and_output_overwrite_refused(self):
        source=self.root/'pack.zip';source.write_bytes(zip_bytes({**self.files,'game.nes':self.image}))
        with self.assertRaisesRegex(ValueError,'ROM images'): self.convert(source)
        source.write_bytes(zip_bytes(self.files))
        with self.assertRaisesRegex(ValueError,'ROM images'): self.convert(source,notice=[self.rom])
        output=self.root/'existing.nesmod';output.write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError,'already exists'): self.convert(source,output.name)
        self.assertEqual(output.read_bytes(),b'keep')


if __name__=='__main__': unittest.main()
