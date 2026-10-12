"""Loader-input regression cases; no game or Windows DLL is executed."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import runtime_dependencies as runtime


def pe(imports=(), delay=(), machine=0x8664):
    data=bytearray(4096)
    data[:2]=b'MZ';struct.pack_into('<I',data,0x3c,0x80)
    data[0x80:0x84]=b'PE\0\0';struct.pack_into('<HH',data,0x84,machine,1)
    struct.pack_into('<H',data,0x94,240)
    optional=0x98
    struct.pack_into('<H',data,optional,0x20b)
    struct.pack_into('<Q',data,optional+24,0x140000000)
    struct.pack_into('<I',data,optional+60,0x400)
    struct.pack_into('<I',data,optional+108,16)
    struct.pack_into('<IIII',data,optional+240+8,0x800,0x1000,0x800,0x400)
    for index, names, stride, rva, name_rva in ((1,imports,20,0x1000,0x1200),(13,delay,32,0x1400,0x1600)):
        if not names:continue
        struct.pack_into('<II',data,optional+112+index*8,rva,(len(names)+1)*stride)
        for number,name in enumerate(names):
            raw=name.encode('ascii')+b'\0';offset=0x400+name_rva-0x1000
            data[offset:offset+len(raw)]=raw
            descriptor=0x400+rva-0x1000+number*stride
            if index==1:struct.pack_into('<IIIII',data,descriptor,1,0,0,name_rva,1)
            else:struct.pack_into('<IIIIIIII',data,descriptor,1,name_rva,0,1,1,0,0,0)
            name_rva+=len(raw)
    return data


class RuntimeDependencies(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='p9-runtime-dependencies-')
        self.root=Path(self.temporary.name);self.package=self.root/'package';self.package.mkdir()
        self.system=self.root/'System32';self.system.mkdir()
        (self.system/'kernel32.dll').write_bytes(pe())

    def tearDown(self):self.temporary.cleanup()

    def binary(self,name,imports=(),delay=(),machine=0x8664):
        path=self.package/name;path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(pe(imports,delay,machine));return path

    def check(self):return runtime.runtime_closure(self.package,self.system,require_plugins=False)

    def test_case_insensitive_transitive_cycles_and_api_sets(self):
        self.binary('openmw.exe',['Shared.dll','KERNEL32.dll','api-ms-win-crt-runtime-l1-1-0.dll'])
        self.binary('shared.dll',['other.dll']);self.binary('other.dll',['Shared.dll'])
        result=self.check()
        self.assertEqual(len(result['files']),3)
        self.assertEqual(result['providers']['shared.dll'],'shared.dll')
        self.assertEqual(result['providers']['kernel32.dll'],'system32/kernel32.dll')
        self.assertEqual(result['providers']['api-ms-win-crt-runtime-l1-1-0.dll'],'windows_api_set')

    def test_missing_direct_and_transitive_import(self):
        self.binary('openmw.exe',['lost.dll'])
        with self.assertRaisesRegex(ValueError,'Missing runtime DLL lost.dll'):self.check()
        self.binary('lost.dll',['inner.dll'])
        with self.assertRaisesRegex(ValueError,'inner.dll required by lost.dll'):self.check()

    def test_delay_import_is_required(self):
        self.binary('openmw.exe',delay=['later.dll'])
        with self.assertRaisesRegex(ValueError,'later.dll'):self.check()
        self.binary('later.dll')
        self.assertEqual(len(self.check()['files']),2)

    def test_osg_plugins_are_separate_dynamic_seeds(self):
        self.binary('openmw.exe')
        with self.assertRaisesRegex(ValueError,'OSG plugin bundle'):
            runtime.runtime_closure(self.package,self.system)
        self.binary('osgPlugins-3.6.5/osgdb_dds.dll',['decode.dll'])
        with self.assertRaisesRegex(ValueError,'decode.dll'):runtime.runtime_closure(self.package,self.system)
        self.binary('decode.dll')
        self.assertEqual(len(runtime.runtime_closure(self.package,self.system)['files']),3)

    def test_mixed_architecture_dependency_refused(self):
        self.binary('openmw.exe',['wrong.dll']);self.binary('wrong.dll',machine=0x14c)
        with self.assertRaisesRegex(ValueError,'AMD64'):self.check()

    def test_truncated_header_and_unbacked_directory(self):
        path=self.binary('openmw.exe');path.write_bytes(b'MZ')
        with self.assertRaisesRegex(ValueError,'Truncated PE'):runtime.imported_dlls(path)
        data=pe(['other.dll']);struct.pack_into('<I',data,0x98+112+8,0x77777777);path.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'Unbacked PE'):runtime.imported_dlls(path)

    def test_invalid_basename_and_unterminated_descriptor(self):
        path=self.binary('openmw.exe',['../escape.dll'])
        with self.assertRaisesRegex(ValueError,'basename'):runtime.imported_dlls(path)
        data=pe(['valid.dll']);struct.pack_into('<I',data,0x98+112+8+4,20);path.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'Unterminated PE import descriptors'):runtime.imported_dlls(path)

    def test_matrix_does_not_launch_with_missing_runtime(self):
        self.binary('openmw.exe',['missing.dll']);self.binary('osgPlugins-3.6.5/osgdb_dds.dll')
        (self.package/'BUILD-IDENTITY.json').write_text('{}')
        symbols=self.root/'symbols.pdb';symbols.write_bytes(b'symbol identity fixture')
        catalog=self.root/'routes.json';catalog.write_text('{"routes":[]}')
        plan=self.root/'plan.json';plan.write_text(json.dumps({'game_directory':str(self.package),
            'symbols':str(symbols),'route_catalog':str(catalog),'cases':[]}))
        spec=importlib.util.spec_from_file_location('runtime_matrix',Path(__file__).with_name('benchmark_matrix.py'))
        matrix=importlib.util.module_from_spec(spec);spec.loader.exec_module(matrix)
        closure=runtime.runtime_closure
        with patch.object(matrix.runtime_dependencies,'runtime_closure',side_effect=lambda p:
                          closure(p,self.system)), patch.object(matrix.benchmark_desktop,'launch') as launch:
            with self.assertRaisesRegex(ValueError,'missing.dll'):matrix.run_matrix(plan,self.root/'output')
            launch.assert_not_called()


if __name__=='__main__':unittest.main()
