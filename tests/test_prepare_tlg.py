"""The experimental target must fail closed, independently of legacy overrides."""
from paths import ROOT
import os
from pathlib import Path
import unittest
from unittest.mock import patch

import prepare_tlg


class TargetValidationTests(unittest.TestCase):
    def check(self, title, version, image):
        with patch.object(Path, 'read_bytes', return_value=b'fixture'), \
             patch.object(prepare_tlg, 'sfo', return_value={'TITLE_ID': title, 'APP_VER': version}):
            return prepare_tlg.validate(Path('fixture'), image)

    def test_observed_identity_and_image_are_accepted_for_preparation_only(self):
        self.assertIsNone(self.check('CUSA03627', '01.03', prepare_tlg.IMAGE))

    def test_other_region_and_old_update_are_rejected_even_with_same_image(self):
        for title, version in [('CUSA03745', '01.03'), ('CUSA03627', '01.00')]:
            with self.subTest(title=title, version=version), self.assertRaises(ValueError):
                self.check(title, version, prepare_tlg.IMAGE)

    def test_mismatched_image_is_rejected_despite_legacy_skip_flag(self):
        with patch.dict(os.environ, {'BB_SKIP_GAME_CHECK': '1'}), self.assertRaises(ValueError):
            self.check('CUSA03627', '01.03', '0' * 64)


if __name__ == '__main__':
    unittest.main()
