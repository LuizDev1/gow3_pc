"""Execute synthetic guest code only; no game files are used by CI."""
from paths import ROOT
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from windows_boot import encode_boot

EXE = Path(os.environ.get('TLG_ENTRY_PROBE', str(ROOT / 'out/tlg-entry-probe.exe')))


def package(code, relocs=(), imports=(), flags=5, capabilities=0):
    image = code.ljust(4096, b'\0')
    return (struct.pack('<8s6Q', b'BBPROBE2', len(image), 0, 1, len(relocs), len(imports), capabilities)
            + struct.pack('<3Q', 0, len(image), flags)
            + b''.join(name.encode().ljust(128, b'\0') for name in imports)
            + b''.join(struct.pack('<4Q', *r) for r in relocs) + image)


def canonical(raw, names, library='libc', version=1, module='libc', module_version=257, kind='function'):
    return encode_boot(raw, dict(schema_version=1, imports=[dict(nid=name.split('#')[0],
        library={'name': library, 'version': version}, module={'name': module, 'version': module_version},
        kind=kind, references=[{'source': 'eboot.bin', 'symbol': name}]) for name in names]))


@unittest.skipUnless(os.name == 'nt' and EXE.is_file(), 'build the Windows entry probe first')
class WindowsEntryTests(unittest.TestCase):
    def test_local_trophy_objects_bind_a_package_without_registration(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'sce_sys/trophy/trophy00.trp'
            path.parent.mkdir(parents=True)
            path.write_bytes(struct.pack('>IIQIII',0xdca24d00,3,128,1,64,0).ljust(128,b'\0'))
            env=dict(os.environ,TLG_GAME_ROOT=directory)
            result=subprocess.run([str(EXE.resolve()),'--selftest-trophy-lifecycle'],env=env,
                                  capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('unload cleanup passed; no registration/unlock',result.stdout)
            before=path.read_bytes()
            self.assertEqual(len(before),128)
            for invalid in (b'bad'.ljust(128,b'\0'),
                            struct.pack('>IIQIII',0xdca24d00,3,256,1,64,0).ljust(128,b'\0')):
                path.write_bytes(invalid)
                result=subprocess.run([str(EXE.resolve()),'--selftest-trophy-lifecycle'],env=env,
                                      capture_output=True,text=True,timeout=15)
                self.assertNotEqual(result.returncode,0)
                self.assertEqual(path.read_bytes(),invalid)

    def test_trophy_canonical_identity_gate(self):
        code=(b'\x31\xff\x48\x83\xec\x08\xff\x15\x14\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x12\0\0\0')
        names=['q7U6tEAQf7c#N#O','after-trp']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libSceNpTrophy',module='libSceNpTrophy')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-trp',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: q7U6tEAQf7c',result.stdout)
    def test_np_title_local_configuration(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-np-title'],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('secret cleanup passed; no online authentication',result.stdout)

    def test_np_title_canonical_identity_gate(self):
        code=(b'\x31\xff\x31\xf6\x48\x83\xec\x08\xff\x15\x12\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x10\0\0\0')
        names=['Ec63y59l9tw#R#S','after-np']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libSceNpManager',module='libSceNpManager')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-np',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: Ec63y59l9tw',result.stdout)
    def test_save_memory_survives_process_restart_without_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            saved=Path(directory)/'CUSA03627-user1-slot0.memory'
            saved.write_bytes(b'KEEP'+b'\0'*12)
            env=dict(os.environ,TLG_SAVE_ROOT=directory)
            for operation in ('write','read'):
                result=subprocess.run([str(EXE.resolve()),'--selftest-save-memory-'+operation],env=env,
                                      capture_output=True,text=True,timeout=15)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(saved.read_bytes(),b'KEEP'+b'\0'*12+b'DATA'+b'\0'*44)
    def test_thread_name_canonical_identity_gate(self):
        code=(b'\x31\xff\x31\xf6\x48\x83\xec\x08\xff\x15\x12\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x10\0\0\0')
        names=['How7B8Oet6k#U#H','after-thread-name']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libkernel',module='libkernel')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-thread-name',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: How7B8Oet6k',result.stdout)
    def test_signal_mask_canonical_identity_gate(self):
        code=(b'\x31\xff\xbe\x01\0\0\0\x31\xd2\x48\x83\xec\x08'
              b'\xff\x15\x0d\0\0\0\x48\x83\xc4\x08\xff\x25\x0b\0\0\0')
        names=['6xVpy0Fdq+I#U#H','after-mask']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libkernel',module='libkernel')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-mask',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: 6xVpy0Fdq+I',result.stdout)
    def test_eventflag_waiter_lifetime_and_bit_consumption(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-eventflag'],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('cancellation and deletion wake passed',result.stdout)

    def test_eventflag_canonical_identity_gate(self):
        code=(b'\x31\xff\x48\x83\xec\x08\xff\x15\x14\0\0\0\x48\x83\xc4\x08'
              b'\x3d\x16\0\x02\x80\x75\x06\xff\x25\x13\0\0\0\x0f\x0b')
        names=['BpFoboUJoZU#U#H','after-flag']
        raw=package(code,[(32,1,0,0),(48,1,1,0)],names,capabilities=1)
        valid=dict(library='libkernel',module='libkernel')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-flag',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: BpFoboUJoZU',result.stdout)

    def test_posix_wrappers_return_errno_and_require_canonical_scope(self):
        code=(b'\x31\xff\x48\x83\xec\x08\xff\x15\x14\0\0\0\x48\x83\xc4\x08'
              b'\x83\xf8\x16\x75\x06\xff\x25\x15\0\0\0\x0f\x0b')
        valid=dict(library='libScePosix',module='libkernel')
        for nid in ('dQHWEsJtoE4','wtkt-teR1so'):
            names=[nid+'#G#H','after-posix']
            raw=package(code,[(32,1,0,0),(48,1,1,0)],names,capabilities=1)
            result=self.run_image(canonical(raw,names,**valid))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: after-posix',result.stdout)
            for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
                result=self.run_image(canonical(raw,names,**(valid|mismatch)))
                self.assertEqual(result.returncode,20,result.stdout+result.stderr)
                self.assertIn('first unsupported PS4 import: '+nid,result.stdout)

    def test_save_initialization_lifecycle_without_persistence_claim(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-save-lifecycle'],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('NULL initialization, idempotency, opaque parameter rejection and termination passed',result.stdout)

    def test_save_initialization_canonical_scope_and_version_gate(self):
        code=(b'\x31\xff\x48\x83\xec\x08\xff\x15\x14\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x12\0\0\0')
        names=['TywrFKCoLGY#L#M','after-save']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libSceSaveData',module='libSceSaveData')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('Runtime: SaveData lifecycle initialized',result.stdout)
        self.assertIn('first unsupported PS4 import: after-save',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            with self.subTest(mismatch=mismatch):
                result=self.run_image(canonical(raw,names,**(valid|mismatch)))
                self.assertEqual(result.returncode,20,result.stdout+result.stderr)
                self.assertNotIn('Runtime: SaveData lifecycle initialized',result.stdout)

    def test_pad_lifecycle_sample_bounds_and_disconnect(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-pad'],capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('lifecycle, sample bounds, conversion, disconnect/reconnect, stale handles and rumble errors passed',result.stdout)

    def test_pad_canonical_identity_and_version_gate(self):
        code=(b'\x48\x83\xec\x08\xff\x15\x16\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x14\0\0\0')
        names=['hv1luiJrqQM#F#G','after-pad']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libScePad',module='libScePad')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('Runtime: Pad library initialized',result.stdout)
        self.assertIn('first unsupported PS4 import: after-pad',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            with self.subTest(mismatch=mismatch):
                result=self.run_image(canonical(raw,names,**(valid|mismatch)))
                self.assertEqual(result.returncode,20,result.stdout+result.stderr)
                self.assertNotIn('Runtime: Pad library initialized',result.stdout)

    def test_user_service_profile_and_concurrent_event_delivery(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-user-service'],
                              capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('local profile, lifecycle, bounded outputs and exactly-once concurrent login event passed',result.stdout)

    def test_user_service_canonical_identity_and_version_gate(self):
        code=(b'\x31\xff\x48\x83\xec\x08\xff\x15\x14\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x12\0\0\0')
        names=['j3YMu1MVNNo#M#N','after-user']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libSceUserService',module='libSceUserService')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('offline local profile: user 1, Player',result.stdout)
        self.assertIn('first unsupported PS4 import: after-user',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            with self.subTest(mismatch=mismatch):
                result=self.run_image(canonical(raw,names,**(valid|mismatch)))
                self.assertEqual(result.returncode,20,result.stdout+result.stderr)
                self.assertNotIn('Runtime: UserService initialized',result.stdout)
                self.assertIn('first unsupported PS4 import: j3YMu1MVNNo',result.stdout)

    def test_windows_thread_name_notification_is_continuable(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-thread-notification'],
                              capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('continuable name event returned normally passed',result.stdout)

    def test_renderer_fault_recovery(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-renderer-recovery'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('protected read resumed at Win64 landing point passed', result.stdout)

    def test_gnm_debug_registration_returns_retail_error(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-gnm-registration'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('retail failure code and unchanged output passed', result.stdout)

    def test_logical_video_port_lifecycle_and_resolution(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-video'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('logical lifecycle, resolution ABI, duplicate open and stale handles passed', result.stdout)
        self.assertIn('no HDR advertised, eight-byte output and invalid-handle preservation passed', result.stdout)

    def test_dialog_provider_lifecycle_has_no_fake_completion(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-dialog'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('load references, initialization order, duplicate init and final unload reset passed', result.stdout)

    def test_guest_thread_creation_tls_and_join(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-thread-create'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('SysV entry, private TLS, stable identity, join result and stale join passed', result.stdout)
        self.assertIn('POSIX threads: named worker, private TLS, join result and positive errno passed',result.stdout)
        self.assertIn('actual stack and Windows bounds, TLS, fault recovery, overlap rejection and reuse passed', result.stdout)

    def test_playgo_initialization_validates_metadata_and_abi(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)/'sce_sys'
            directory.mkdir()
            data = bytearray(20)
            data[:4] = b'plgo'
            struct.pack_into('<H', data, 10, 27)
            struct.pack_into('<I', data, 16, 20)
            (directory/'playgo-chunk.dat').write_bytes(data)
            result = subprocess.run([str(EXE.resolve()), '--selftest-playgo'], env=dict(os.environ, TLG_GAME_ROOT=root, TLG_PLAYGO_ASSUME_LOCAL='0'), capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('metadata initialization, ABI validation and open lifecycle passed', result.stdout)
            enabled = subprocess.run([str(EXE.resolve()), '--selftest-playgo'], env=dict(os.environ, TLG_GAME_ROOT=root, TLG_PLAYGO_ASSUME_LOCAL='1'), capture_output=True, text=True, timeout=15)
            self.assertEqual(enabled.returncode, 0, enabled.stdout + enabled.stderr)
            self.assertIn('OFFLINE ASSUMPTION', enabled.stdout)
            self.assertIn('explicit profile, invalid IDs and unchanged error outputs passed', enabled.stdout)
            (directory/'playgo-chunk.dat').write_bytes(b'bad metadata')
            rejected = subprocess.run([str(EXE.resolve()), '--selftest-playgo'], env=dict(os.environ, TLG_GAME_ROOT=root, TLG_PLAYGO_ASSUME_LOCAL='0'), capture_output=True, text=True, timeout=15)
            self.assertNotEqual(rejected.returncode, 0)

    def test_semaphore_counted_wait_and_concurrent_deletion(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-semaphore'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('counted FIFO wait, timeout, isolation, overflow and deletion wake passed', result.stdout)

    def test_system_language_profile_and_invalid_requests(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-system-parameter'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('explicit language profile and invalid-output preservation passed', result.stdout)

    def test_system_status_output_bounds_and_profile(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-system-status'],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('nine-byte fields and reserved-output preservation passed',result.stdout)

    def test_system_splash_canonical_identity_gate(self):
        code=(b'\x48\x83\xec\x08\xff\x15\x16\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x14\0\0\0')
        names=['Vo5V8KAwCmk#W#W','after-hide']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libSceSystemService',module='libSceSystemService')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-hide',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: Vo5V8KAwCmk',result.stdout)

    def test_system_status_canonical_identity_gate(self):
        code=(b'\x31\xff\x48\x83\xec\x08\xff\x15\x14\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x12\0\0\0')
        names=['rPo6tV8D9bM#W#W','after-sys']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libSceSystemService',module='libSceSystemService')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-sys',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: rPo6tV8D9bM',result.stdout)

    def test_appcontent_lifecycle_and_output_bounds(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-content'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('references, initialization bounds, invalid profile and unload reset passed', result.stdout)

    def test_readonly_files_preserve_data_and_descriptor_lifecycle(self):
        with tempfile.TemporaryDirectory() as root:
            fixture = Path(root)/'fixture.bin'
            fixture.write_bytes(b'ABCDE')
            env = dict(os.environ, TLG_GAME_ROOT=root)
            result = subprocess.run([str(EXE.resolve()), '--selftest-files'], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(fixture.read_bytes(), b'ABCDE')
            self.assertIn('exact bytes, seek, EOF and stale descriptors passed', result.stdout)
            self.assertIn('120-byte ABI, size, type, timestamp normalization and unchanged errors passed', result.stdout)

    def test_guest_clock_layout_translation_and_errors(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-timespec'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Unix epoch, SECOND truncation, monotonic advance, aliases and invalid inputs passed', result.stdout)

    def test_timing_uses_advancing_counter_and_consistent_frequency(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-clock'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('advancing hardware counter, stable frequency and elapsed-time agreement passed', result.stdout)

    def test_process_counter_exports_require_kernel_identity(self):
        code=(b'\x48\x83\xec\x08\xff\x15\x16\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x14\0\0\0')
        for symbol in ['fgxnMeTNUtY', 'BNowx2l588E']:
            names=[symbol+'#U#H', 'after-clock']
            raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
            valid=dict(library='libkernel',module='libkernel')
            result=self.run_image(canonical(raw,names,**valid))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: after-clock',result.stdout)
            for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
                result=self.run_image(canonical(raw,names,**(valid|mismatch)))
                self.assertEqual(result.returncode,20,result.stdout+result.stderr)
                self.assertIn('first unsupported PS4 import: '+symbol,result.stdout)

    def test_discmap_module_reports_absent_bitmap_without_modifying_outputs(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-discmap'],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('absent bitmap and unchanged outputs passed',result.stdout)

    def test_discmap_canonical_identity_gate(self):
        code=(b'\x48\x83\xec\x08\xff\x15\x16\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x14\0\0\0')
        names=['lbQKqsERhtE#D#D','after-disc']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libSceDiscMap',module='libSceDiscMap')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-disc',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: lbQKqsERhtE',result.stdout)

    def test_kernel_clock_requires_kernel_identity(self):
        code=(b'\x31\xff\x31\xf6\x48\x83\xec\x08\xff\x15\x12\0\0\0'
              b'\x48\x83\xc4\x08\xff\x25\x10\0\0\0')
        names=['QBi7HCK03hw#U#H','after-clock']
        raw=package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1)
        valid=dict(library='libkernel',module='libkernel')
        result=self.run_image(canonical(raw,names,**valid))
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-clock',result.stdout)
        for mismatch in [dict(library='other'),dict(module='other'),dict(version=2),dict(module_version=258)]:
            result=self.run_image(canonical(raw,names,**(valid|mismatch)))
            self.assertEqual(result.returncode,20,result.stdout+result.stderr)
            self.assertIn('first unsupported PS4 import: QBi7HCK03hw',result.stdout)

    def test_console_preserves_binary_data_and_routes_descriptors(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-console'], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'TLG\0\n')
        self.assertEqual(result.stderr, b'console stderr\n')

    def test_thread_attribute_lifecycle_and_validation(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-thread'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('defaults, validation, isolation, round trips and stale handles passed', result.stdout)
        self.assertIn('actual reserved stack, output isolation and invalid handles passed', result.stdout)
        self.assertIn('actual Windows CPU mask, restoration and invalid-handle rejection passed',result.stdout)
        self.assertIn('Windows relative level, guest-value query, restoration and invalid-handle rejection passed',result.stdout)

    def test_direct_memory_shared_storage_and_lifecycle(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-memory'], capture_output=True, text=True, timeout=15)
        self.assertIn('Renderer memory queries: view bounds, gaps, overflow and unmap invalidation passed', result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('shared aliases, low addresses and lifecycle passed', result.stdout)
        self.assertIn('no-overwrite alias placement and GPU permission metadata passed', result.stdout)
        self.assertIn('zero-fill, isolation, budget, invalid requests and release passed', result.stdout)
        self.assertIn('initial budget, allocation debit and release credit passed', result.stdout)

    def test_console_profile_reports_base_ps4_to_guest(self):
        code=(b'\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08'
              b'\x85\xc0\x75\x06\xff\x25\x10\0\0\0\x0f\x0b')
        names=['WslcK1FQcGI#U#H','after-neo#U#H']
        data=canonical(package(code,[(32,1,0,0),(40,1,1,0)],names,capabilities=1),names,
                       library='libkernel',module='libkernel')
        result=self.run_image(data)
        self.assertEqual(result.returncode,20,result.stdout+result.stderr)
        self.assertIn('first unsupported PS4 import: after-neo',result.stdout)

    def test_heap_trace_exposes_shared_storage_without_overwriting_input_fields(self):
        result=subprocess.run([str(EXE.resolve()),'--selftest-heap-trace'],
                              capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('ABI fields, shared writable mask and 64-entry table passed',result.stdout)

    def test_windows_tls_uses_real_slot_and_isolates_host_threads(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-tls'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('actual GS slot read, template, zero-fill, thread isolation and detach passed', result.stdout)
        self.assertIn('stable self, TCB agreement, host-thread isolation and detach passed', result.stdout)

    def test_canonical_imports_resolve_independent_of_local_suffix(self):
        code = (b'\x48\x83\xec\x08\xff\x15\x16\0\0\0'
                b'\x48\x83\xc4\x08\xff\x25\x14\0\0\0')
        names = ['bzQExy189ZI#X#Y', 'after-init#X#Y']
        result = self.run_image(canonical(package(code, [(32,1,0,0),(40,1,1,0)], names,
                                                  capabilities=1), names))
        self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
        self.assertIn('Verified _init_env calls: 1', result.stdout)
        self.assertIn('first unsupported PS4 import: after-init', result.stdout)

    def test_canonical_scope_and_versions_cannot_resolve_same_nid(self):
        names = ['bzQExy189ZI#V#V']
        raw = package(b'\xff\x25\x02\0\0\0\x90\x90', [(8,1,0,0)], names, capabilities=1)
        for options in [dict(library='other'), dict(module='other'), dict(version=2), dict(module_version=258)]:
            with self.subTest(options=options):
                result = self.run_image(canonical(raw, names, **options))
                self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
                self.assertIn('Verified _init_env calls: 0', result.stdout)

    def test_canonical_function_data_mismatch_fails_before_execution(self):
        names = ['bzQExy189ZI#V#V']
        raw = package(b'\xff\x25\x02\0\0\0\x90\x90', [(8,1,0,0)], names, capabilities=1)
        result = self.run_image(canonical(raw, names, kind='data'))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('canonical symbol kind mismatch', result.stderr)
        self.assertNotIn('Entering original', result.stdout)

    def test_mutex_attribute_lifecycle_and_invalid_handles(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-mutex'],
                                env=dict(os.environ, TLG_MUTEX_PRIORITY_FALLBACK='0'),
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('POSIX mutexes: shared kernel objects, recursive ownership and positive errno passed',result.stdout)
        self.assertIn('type validation, isolation and stale handles passed', result.stdout)
        self.assertIn('concurrent readers, exclusive writer, ownership, recursion and stale handles passed', result.stdout)
        self.assertIn('zero slots create independent owned locks', result.stdout)
        self.assertIn('NONE supported, priority protocols explicitly rejected and handles validated', result.stdout)
        self.assertIn('ownership, recursion, contention, busy destruction and stale handles passed', result.stdout)
        self.assertIn('condition lifecycle: attributes, defaults, isolation and stale handles passed', result.stdout)
        self.assertIn('atomic mutex release, signal, broadcast, reacquisition and busy lifetime passed', result.stdout)

    def test_mutex_priority_fallback_is_explicit_and_preserves_locking(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-mutex'], env=dict(os.environ, TLG_MUTEX_PRIORITY_FALLBACK='1'), capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('explicit fallback selected, stored on lock and priority ceilings rejected', result.stdout)
        self.assertIn('ownership, recursion, contention, busy destruction and stale handles passed', result.stdout)

    def test_event_queue_lifecycle_and_invalid_handles(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-equeue'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('lifecycle, invalid handles, invalid inputs and exact scope checks passed', result.stdout)
        self.assertIn('replacement, isolation, explicit signaling and deletion checks passed', result.stdout)

    def test_atexit_callbacks_finalize_in_reverse_order_exactly_once(self):
        result = subprocess.run([str(EXE.resolve()), '--selftest-atexit'],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('exactly-once checks passed', result.stdout)
        self.assertIn('C++ destructors: argument forwarding and exactly-once checks passed', result.stdout)

    def run_image(self, data):
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / 'fixture.bin'
            image.write_bytes(data)
            return subprocess.run([str(EXE.resolve()), str(image)], capture_output=True,
                                  text=True, timeout=15)

    def test_original_entry_reaches_named_import(self):
        result = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90',
                                        [(8, 1, 0, 0)], ['fixture-import']))
        self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
        self.assertIn('first unsupported PS4 import: fixture-import', result.stdout)

    def test_unsupported_data_import_is_named(self):
        code = b'\x48\x8b\x05\x09\0\0\0\x48\x8b\x00\xc3'
        result = self.run_image(package(code, [(16, 2, 0, 0)], ['fixture-data']))
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertIn('Unsupported data import: fixture-data', result.stdout)

    def test_verified_init_env_returns_then_reaches_next_import(self):
        # Align the SysV stack, call the first import, then tail-jump to the second.
        code = (b'\x48\x83\xec\x08\xff\x15\x16\0\0\0'
                b'\x48\x83\xc4\x08\xff\x25\x14\0\0\0')
        result = self.run_image(package(code, [(32, 1, 0, 0), (40, 1, 1, 0)],
                                       ['bzQExy189ZI#V#V', 'after-init'], capabilities=1))
        self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
        self.assertIn('Verified _init_env calls: 1', result.stdout)
        self.assertIn('first unsupported PS4 import: after-init', result.stdout)

    def test_init_env_requires_preparation_evidence_and_exact_scope(self):
        for name, capabilities in [('bzQExy189ZI#V#V', 0), ('bzQExy189ZI#q#q', 1)]:
            with self.subTest(name=name, capabilities=capabilities):
                result = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90',
                                                [(8, 1, 0, 0)], [name], capabilities=capabilities))
                self.assertEqual(result.returncode, 20, result.stdout + result.stderr)
                self.assertIn('Verified _init_env calls: 0', result.stdout)
                self.assertNotIn('Runtime: _init_env returned', result.stdout)

    def test_guest_fault_reports_offset(self):
        result = self.run_image(package(b'\x0f\x0b'))
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertIn('guest offset 0x0', result.stdout)

    def test_verified_stack_canary_data_is_readable(self):
        code = b'\x48\x8b\x05\x09\0\0\0\x48\x8b\x00\x0f\x0b'
        result = self.run_image(package(code, [(16, 2, 0, 0)],
                                       ['f7uOxY9mM1U#U#H'], capabilities=1))
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertIn('guest offset 0xa', result.stdout)
        self.assertNotIn('Unsupported data import', result.stdout)

    def test_bad_images_fail_before_execution(self):
        cases = [package(b'\xc3')[:-1], package(b'\xc3') + b'extra',
                 package(b'\xc3', flags=4),
                 package(b'\xc3', [(4090, 0, 0, 0)]),
                 b'BBPROBE5' + package(b'\xc3')[8:]]
        for data in cases:
            with self.subTest(header=data[:8], length=len(data)):
                result = self.run_image(data)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertNotIn('Entering original', result.stdout)


if __name__ == '__main__':
    unittest.main()
