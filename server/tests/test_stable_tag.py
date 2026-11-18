"""Run with python3 -m unittest discover -s server/tests."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class StableTagTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('config', 'user.name', 'Recipe test')
        self.git('commit', '-q', '--allow-empty', '-m', 'fixture')
        for tag in ['v0.4', 'v0.5', 'v0.5dev2', 'v0.6dev1']:
            self.git('tag', tag)
        script = Path(__file__).resolve().parents[1] / 'build.sh'
        functions = script.read_text().split('\ncase "$1" in\n')[0]
        self.functions = self.root / 'functions.sh'
        self.functions.write_text(functions)
        self.calls = self.root / 'calls'

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.source), *args], check=True,
                              capture_output=True, text=True)

    def run_recipes(self, options):
        commands = []
        for number, settings in enumerate(options):
            recipe = self.root / ('pkg%d.pkg' % number)
            recipe.write_text('GIT=%s\nBUILD=true\nFILES=x:/bin/x\n%s\n' %
                              (shlex.quote(str(self.source)), settings))
            commands.append('build_recipe %s force || exit $?' % shlex.quote(str(recipe)))
        script = '''export WORK=%s REPO=%s
. %s
mkdir -p "$WORK/src"
build_one() { printf '%%s %%s %%s\\n' "$1" "$2" "$3" >> %s; }
%s
''' % (shlex.quote(str(self.root/'work')), shlex.quote(str(self.root/'repo')),
       shlex.quote(str(self.functions)), shlex.quote(str(self.calls)), '\n'.join(commands))
        result = subprocess.run(['bash'], input=script, capture_output=True, text=True)
        calls = self.calls.read_text().splitlines() if self.calls.exists() else []
        return result, calls

    def test_default_picks_newest_release_over_prereleases(self):
        result, calls = self.run_recipes([''])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0], 'pkg0 v0.5 0.5')
        self.assertIn('origin/HEAD', calls[1])

    def test_release_tag_shapes(self):
        for tag in ['v0.5dev2', 'v0.6dev1']:
            self.git('tag', '-d', tag)
        for tag in ['v0.5-dev1', 'v0.6rc1', 'v0.6-beta2', 'release-0.7', 'v0.10', '0.9']:
            self.git('tag', tag)
        result, calls = self.run_recipes([''])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0], 'pkg0 v0.10 0.10')

    def test_only_prerelease_tags_publishes_nightly_only(self):
        for tag in ['v0.4', 'v0.5']:
            self.git('tag', '-d', tag)
        result, calls = self.run_recipes([''])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('no release tags yet', result.stdout)
        self.assertEqual(len(calls), 1)
        self.assertIn('origin/HEAD', calls[0])

    def test_pin_overrides_newest_release(self):
        result, calls = self.run_recipes(['STABLE_TAG=v0.4'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls[0], 'pkg0 v0.4 0.4')

    def test_pin_may_name_a_prerelease_tag(self):
        result, calls = self.run_recipes(['STABLE_TAG=v0.6dev1'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls[0], 'pkg0 v0.6dev1 0.6dev1')

    def test_missing_pin_fails_before_building(self):
        result, calls = self.run_recipes(['STABLE_TAG=v9.9'])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('stable tag v9.9 does not exist', result.stdout)
        self.assertEqual(calls, [])

    def test_stable_no_takes_precedence(self):
        result, calls = self.run_recipes(['STABLE=no\nSTABLE_TAG=v9.9'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 1)
        self.assertIn('origin/HEAD', calls[0])

    def test_override_does_not_leak_between_recipes(self):
        result, calls = self.run_recipes(['STABLE_TAG=v0.4', ''])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0], 'pkg0 v0.4 0.4')
        self.assertEqual(calls[2], 'pkg1 v0.5 0.5')


if __name__ == '__main__':
    unittest.main()
