"""Exercise the real shell script with a local GitHub API fake and real jq."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'comment-desktop-build.sh'
SHA = 'a' * 40
MARKER = '<!-- desktop-build-downloads run={} -->\n\nold body'
BOT = {'login': 'github-actions[bot]', 'type': 'Bot'}
FAKE_GH = r'''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys
path = pathlib.Path(os.environ['FAKE_STATE'])
state = json.loads(path.read_text())
args = sys.argv[1:]
endpoint = next(a for a in args if a.startswith('repos/'))
method = args[args.index('-X') + 1] if '-X' in args else 'GET'
if state.get('fail'):
    sys.exit(1)
if method in ('POST', 'PATCH'):
    body = next(a[5:] for a in args if a.startswith('body='))
    if method == 'POST':
        state['comments'].append({'id': 100, 'body': body, 'user':
                                 {'login': 'github-actions[bot]', 'type': 'Bot'}})
    else:
        comment = next(c for c in state['comments'] if c['id'] == int(endpoint.split('/')[-1]))
        comment['body'] = body
    state['writes'].append(method)
    path.write_text(json.dumps(state))
    sys.exit(0)
if '/commits/' in endpoint:
    response = state['prs']
elif endpoint.endswith('/artifacts'):
    response = {'artifacts': state['artifacts']}
elif '/issues/comments/' in endpoint:
    response = next(c for c in state['comments'] if c['id'] == int(endpoint.split('/')[-1]))
elif endpoint.endswith('/comments'):
    response = state['comments']
else:
    raise ValueError(endpoint)
if '--jq' in args:
    result = subprocess.run(['jq', '-r', args[args.index('--jq') + 1]],
                            input=json.dumps(response), text=True)
    sys.exit(result.returncode)
print(json.dumps(response))
'''


class DesktopCommentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        gh = self.root / 'gh'
        gh.write_text(FAKE_GH)
        gh.chmod(0o755)
        self.state_path = self.root / 'state.json'
        self.state = {
            'prs': [{'number': 42, 'state': 'open', 'head': {'sha': SHA},
                     'base': {'repo': {'full_name': 'owner/repo'}}}],
            'artifacts': [{'id': i, 'name': 'disquisition-desktop-' + platform,
                           'expired': False} for i, platform in
                          enumerate(['macos-arm64', 'windows-x64', 'linux-x64'], 1)],
            'comments': [], 'writes': [],
        }

    def run_script(self, run_id=200, result='success', expected=0):
        self.state_path.write_text(json.dumps(self.state))
        env = dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ['PATH'],
                   FAKE_STATE=str(self.state_path), GITHUB_REPOSITORY='owner/repo',
                   GITHUB_SERVER_URL='https://github.com', BUILD_RUN_ID=str(run_id),
                   BUILD_HEAD_SHA=SHA, BUILD_RESULT=result)
        run = subprocess.run(['bash', str(SCRIPT)], env=env, capture_output=True, text=True)
        self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
        self.state = json.loads(self.state_path.read_text())

    def test_create_then_rerun_updates_one_comment_with_all_downloads(self):
        self.run_script()
        self.run_script()
        self.assertEqual(self.state['writes'], ['POST', 'PATCH'])
        self.assertEqual(len(self.state['comments']), 1)
        body = self.state['comments'][0]['body']
        for i in range(1, 4):
            self.assertIn(f'https://github.com/owner/repo/actions/runs/200/artifacts/{i}', body)
        self.assertIn(f'commit `{SHA[:7]}`', body)

    def test_user_marker_cannot_hijack_or_suppress_bot_comment(self):
        self.state['comments'] = [
            {'id': 1, 'user': BOT, 'body': MARKER.format(100)},
            {'id': 2, 'user': {'login': 'person', 'type': 'User'}, 'body': MARKER.format(999)},
            {'id': 3, 'user': BOT, 'body': 'Quoted desktop-build-downloads'},
        ]
        self.run_script()
        self.assertIn('run=200', self.state['comments'][0]['body'])
        self.assertEqual(self.state['comments'][1]['body'], MARKER.format(999))
        self.assertEqual(self.state['comments'][2]['body'], 'Quoted desktop-build-downloads')
        self.assertEqual(self.state['writes'], ['PATCH'])

    def test_older_run_leaves_newer_links_untouched(self):
        self.state['comments'] = [{'id': 1, 'user': BOT, 'body': MARKER.format(300)}]
        self.run_script()
        self.assertEqual(self.state['writes'], [])
        self.assertEqual(self.state['comments'][0]['body'], MARKER.format(300))

    def test_partial_failed_build_shows_missing_and_expired_downloads(self):
        self.state['artifacts'].pop()
        self.state['artifacts'][0]['expired'] = True
        self.run_script(result='failure')
        body = self.state['comments'][0]['body']
        self.assertIn('WARNING', body)
        self.assertIn('| macOS, Apple silicon | Expired |', body)
        self.assertIn('| Linux, x64 | Not available |', body)
        self.assertIn('/artifacts/2)', body)

    def test_superseded_closed_or_other_repository_pr_is_ignored(self):
        original = self.state['prs'][0]
        self.state['prs'] = [dict(original, head={'sha': 'b' * 40}),
                             dict(original, state='closed'),
                             dict(original, base={'repo': {'full_name': 'other/repo'}})]
        self.run_script()
        self.assertEqual(self.state['writes'], [])

    def test_api_failure_fails_without_posting(self):
        self.state['fail'] = True
        self.run_script(expected=1)
        self.assertEqual(self.state['writes'], [])


if __name__ == '__main__':
    unittest.main()
