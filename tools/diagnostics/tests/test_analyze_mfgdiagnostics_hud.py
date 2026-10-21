import json
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_mfgdiagnostics_hud import analyze


class TraceTests(unittest.TestCase):
    def build(self, root: Path, role: str) -> Path:
        root.mkdir(parents=True)
        (root / 'summary.json').write_text(json.dumps({
            'schema': 2, 'dropped_events': 0, 'capacity_exhausted': False}), encoding='utf-8')
        rows = [
            {'schema': 2, 'sequence': 1, 'event': 'streamline_options',
             'options': {'ui_recomposition': 1, 'mode': 1, 'generated_requested': 5}},
            {'schema': 2, 'sequence': 2, 'event': 'resource_tag', 'result': 0,
             'viewport': 7, 'frame_id': 11,
             'resource_tag': {'role': 'HUDLessColor', 'tag_resource_present': True, 'resource': 2}},
            {'schema': 2, 'sequence': 3, 'event': 'resource_tag', 'result': 0,
             'viewport': 7, 'frame_id': 11,
             'resource_tag': {'role': role, 'tag_resource_present': True, 'resource': 3}},
            {'schema': 2, 'sequence': 4, 'event': 'resource_tag', 'result': 0,
             'viewport': 7, 'frame_id': None,
             'resource_tag': {'role': 'UIAlpha', 'tag_resource_present': False, 'resource': None}},
        ]
        (root / 'trace.jsonl').write_text(''.join(json.dumps(x)+'\n' for x in rows), encoding='utf-8')
        return root

    def test_pair_and_comparison(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            a = self.build(base / 'a', 'UIAlpha')
            b = self.build(base / 'b', 'UIColorAndAlpha')
            report = analyze([a, b])
            self.assertEqual(report['sessions'][0]['frame_scoped']['frames_with_observed_pair'], 1)
            self.assertEqual(report['sessions'][1]['frame_scoped']['frames_with_observed_pair'], 1)
            self.assertEqual(report['sessions'][0]['role_counts']['UIAlpha']['clear_tags'], 1)
            self.assertEqual(report['sessions'][0]['role_counts']['UIAlpha']['active_tags'], 1)
            self.assertEqual(report['sessions'][0]['role_counts']['HUDLessColor']['distinct_native_resources'], 1)
            self.assertEqual(report['comparison_right_minus_left']['UIAlpha']['active_tags'], -1)

    def test_no_false_positive_for_unframed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.build(Path(tmp)/'one', 'UIAlpha')
            lines = (root/'trace.jsonl').read_text().splitlines()
            # Only the HUDLessColor tag remains unframed.
            row = json.loads(lines[1]); row['frame_id'] = None
            lines[1] = json.dumps(row)
            (root/'trace.jsonl').write_text('\n'.join(lines)+'\n')
            report = analyze([root])['sessions'][0]
            self.assertEqual(report['frame_scoped']['frames_with_observed_pair'], 0)
            self.assertEqual(report['frame_scoped']['frames_with_partial_observation'], 1)


if __name__ == '__main__':
    unittest.main()
