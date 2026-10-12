import pathlib
import tempfile
import unittest

import image_publication_work as work


class ImagePublicationWorkTests(unittest.TestCase):
    def record(self, *, enabled=1, publications=2, winners=1, images=1, payload=8, ineligible=0):
        return (f'{work.FINAL_PREFIX} ordinary_enabled={enabled} publications={publications} '
                f'winners={winners} discarded_images={images} discarded_payload_bytes_cumulative={payload} '
                f'revision_declines=0 epoch_declines=0 ineligible={ineligible} lookup_revision_misses=0 '
                f'scope={work.PRODUCER_SCOPE}')

    def policy(self, enabled=1):
        return f'{work.POLICY_PREFIX} enabled={enabled} scope={work.POLICY_SCOPE}'

    def supplemental(self, enabled=1):
        return (f'{work.FINAL_PREFIX} enabled={enabled}; concurrent decoded owners only, '
                'not a whole-process RAM ceiling')

    def parse(self, text):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'openmw.log').write_text(text, encoding='utf-8-sig')
            return work.lifetime_work(root)

    def full(self, record=None):
        return '[01:02:03.004 I] ' + self.policy() + '\nnoise\n[01:04:05.006 I] ' + (record or self.record()) + '\n'

    def test_exact_final_positive_lifetime_only(self):
        result = self.parse(self.full())
        self.assertEqual(result['scope'], work.SCOPE)
        self.assertEqual(result['producer_scope'], work.PRODUCER_SCOPE)
        self.assertEqual(result['counts']['discarded_payload_bytes_cumulative'], 8)
        self.assertEqual(result['receipt']['startup_line'], 1)
        self.assertEqual(result['receipt']['shutdown_line'], 3)
        self.assertIn('not decoder peaks', result['limit'])
        self.assertIn('measured-route', result['limit'])
        self.assertTrue(work.positive_engagement(result, requested=True))
        self.assertFalse(work.positive_engagement(result, requested=False))
        self.assertFalse(work.positive_engagement(result, requested=1))

    def test_zero_actual_release_not_positive(self):
        result = self.parse(self.full(self.record(winners=0, images=0, payload=0)))
        self.assertFalse(work.positive_engagement(result, requested=True))

    def test_cold_bypass_is_not_ordinary_engagement(self):
        result = self.parse(self.full(self.record(enabled=0, publications=0, winners=0, images=0, payload=0)))
        self.assertFalse(result['enabled'])
        self.assertFalse(work.positive_engagement(result, requested=True))

    def test_missing_shutdown_is_unavailable_not_zero_work(self):
        with self.assertRaisesRegex(ValueError, 'shutdown'):
            self.parse(self.policy() + '\n')

    def test_ordinary_control_has_no_candidate_shutdown(self):
        with self.assertRaises(ValueError):
            self.parse(self.policy(0) + '\n')

    def test_missing_or_repeated_startup(self):
        for text in (self.record(), self.policy() + '\n' + self.full()):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.parse(text)

    def test_disabled_startup_or_reversed_order_rejected(self):
        for text in (self.policy(0) + '\n' + self.record(), self.record() + '\n' + self.policy()):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.parse(text)

    def test_duplicate_shutdown_same_or_separate_line(self):
        for text in (self.full() + self.record(), self.full(self.record() + ' ' + self.record())):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.parse(text)

    def test_duplicate_missing_unknown_empty_fields(self):
        for record in (self.record() + ' winners=1', self.record().replace('winners=1 ', ''),
                       self.record() + ' guessed_bytes=8', self.record().replace('winners=1', 'winners='),
                       self.record() + ' bare_token', self.record().replace('winners=1', 'winners==1')):
            with self.subTest(record=record), self.assertRaises(ValueError):
                self.parse(self.full(record))

    def test_invalid_numeric_encodings_and_overflow(self):
        for value in ('-1', '+1', '1.0', 'nan', 'inf', '0x10', 'a', '1e2', '\u0661', str(1 << 64), '0' * 21):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.parse(self.full(self.record().replace('winners=1', 'winners=' + value)))

    def test_uint64_boundary_is_supported_without_float_loss(self):
        record = self.record(publications=work.UINT64_MAX, winners=work.UINT64_MAX,
                             images=work.UINT64_MAX, payload=work.UINT64_MAX)
        result = self.parse(self.full(record))
        self.assertEqual(result['counts']['discarded_payload_bytes_cumulative'], work.UINT64_MAX)
        self.assertTrue(work.positive_engagement(result, requested=True))

    def test_counter_inconsistency_rejected(self):
        records = (self.record(images=2), self.record(payload=0), self.record(winners=0, images=0, payload=8),
                   self.record(publications=0), self.record(ineligible=3),
                   self.record(winners=2, images=2, payload=1), self.record(enabled=0))
        for record in records:
            with self.subTest(record=record), self.assertRaises(ValueError):
                self.parse(self.full(record))

    def test_same_object_only_winner_has_no_positive_release(self):
        result = self.parse(self.full(self.record(publications=1, winners=1, images=0, payload=0)))
        self.assertEqual(result['counts']['winners'], 1)
        self.assertEqual(result['counts']['discarded_images'], 0)
        self.assertFalse(work.positive_engagement(result, requested=True))
        # Some accepted winners may refer to existing shared objects while
        # others discard distinct owned decoder payloads in the same lifetime.
        mixed = self.parse(self.full(self.record(publications=1, winners=2, images=1, payload=8)))
        self.assertTrue(work.positive_engagement(mixed, requested=True))

    def test_unknown_boolean_or_scope_rejected(self):
        for record in (self.record(enabled=2), self.record().replace(work.PRODUCER_SCOPE, 'resident_RAM_saved')):
            with self.subTest(record=record), self.assertRaises(ValueError):
                self.parse(self.full(record))
        for policy in (self.policy().replace('enabled=1', 'enabled=true'), self.policy() + ' enabled=1',
                       self.policy().replace(work.POLICY_SCOPE, 'unknown'), self.policy() + ' extra=1'):
            with self.subTest(policy=policy), self.assertRaises(ValueError):
                self.parse(policy + '\n' + self.record())

    def test_malformed_or_missing_file_not_positive(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                work.lifetime_work(directory)
        for unknown in (None, {}, {'counts': {'discarded_images': 1}}, {'scope': 'route', 'enabled': True}):
            self.assertFalse(work.positive_engagement(unknown, requested=True))

    def test_corrupted_result_cannot_be_promoted(self):
        for key, value in (('enabled', 1), ('complete_final_receipt', False), ('startup_enabled', False),
                           ('scope', 'measured_interval'), ('producer_scope', 'resident_RAM_saved')):
            result = self.parse(self.full()); result[key] = value
            self.assertFalse(work.positive_engagement(result, requested=True))
        result = self.parse(self.full()); result['counts']['winners'] = 0
        self.assertFalse(work.positive_engagement(result, requested=True))
        result = self.parse(self.full()); result['counts']['discarded_images'] = True
        self.assertFalse(work.positive_engagement(result, requested=True))

    def test_other_game_receipts_are_not_image_publication(self):
        with self.assertRaises(ValueError):
            self.parse(self.policy() + '\nOptimizedMW cold image cache: compressed=1 restored=1\n')

    def test_actual_game_supplemental_startup_and_final_format(self):
        text = '[01:02:03.004 I] ' + self.policy() + '\n[01:02:03.005 I] ' + self.supplemental() + '\nnoise\n'
        text += '[01:04:05.006 I] ' + self.record() + '\n'
        result = self.parse(text)
        self.assertEqual(result['receipt']['startup_line'], 1)
        self.assertEqual(result['receipt']['supplemental_startup_line'], 2)
        self.assertIs(result['receipt']['supplemental_startup_enabled'], True)
        self.assertEqual(result['receipt']['shutdown_line'], 4)
        self.assertTrue(work.positive_engagement(result, requested=True))

    def test_supplemental_flag_conflicts_are_rejected(self):
        for text in (self.policy() + '\n' + self.supplemental(0) + '\n' + self.record(),
                     self.policy(0) + '\n' + self.supplemental(1) + '\n' + self.record()):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.parse(text)

    def test_actual_game_off_startup_is_not_positive_or_final(self):
        with self.assertRaisesRegex(ValueError, 'shutdown'):
            self.parse(self.policy(0) + '\n' + self.supplemental(0) + '\n')

    def test_supplemental_duplicates_and_late_startup_rejected(self):
        for text in (self.policy() + '\n' + self.supplemental() + '\n' + self.supplemental() + '\n' + self.record(),
                     self.policy() + '\n' + self.supplemental() + '\n' + self.supplemental(0) + '\n' + self.record(),
                     self.full() + self.supplemental(),
                     self.policy() + '\n' + self.supplemental() + ' ' + self.supplemental() + '\n' + self.record()):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.parse(text)

    def test_unknown_final_prefix_cannot_be_filtered_as_startup(self):
        for supplemental in (self.supplemental(2), self.supplemental().replace(';', ''),
                             self.supplemental().replace('ceiling', 'savings'), self.supplemental() + ' extra=1'):
            with self.subTest(supplemental=supplemental), self.assertRaises(ValueError):
                self.parse(self.policy() + '\n' + supplemental + '\n' + self.record())
            with self.subTest(missing_final=supplemental), self.assertRaises(ValueError):
                self.parse(self.policy() + '\n' + supplemental)


if __name__ == '__main__':
    unittest.main()
