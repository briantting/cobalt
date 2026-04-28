#!/usr/bin/env python3
"""Script to automatically roll LTS branch."""
import argparse
import re
import subprocess
import sys

_SKIP_LIST = {
    '27.lts': [
        # Reorders deleted BUILD_STATUS.md file (#9476, #9508).
        '7e6524981fdd6ab3c87bc55785343d40116a05e5',
        'adda40a0d3b08b9302f441e76eef0391c70e0462',
        # Modifies deleted workflow trigger files (#9473, #10110).
        'b24037232cbc7a74bf01dbc4c93dbe9701328b5e',
        '234bf5073b12bcd7a08e44eaeb42f809137a440e',
        # Skia import commits, already applied in #9625 (#9624).
        'b77e86a96022541455c239778a4a62462d790c73',
        '8ed51696a04da8b51b82d6540b3b314347c43794',
        # Change to deleted workflow file (#9670, #9934, #10080, #9593).
        'f69b1d1e21f3340d9c963846ed4e1cbef8fa2fb9',
        '478e5c52cf4872407ed855a100165e93b02d9eee',
        '00531389e019a835f49fb3bf56364b244a0d3acd',
        '9b2c106aa54a05640705a3603ebc6821e1adebf8',
    ],
}


def get_out(cmd):
  res = subprocess.run(cmd, capture_output=True, text=True, check=True)
  return res.stdout


def get_commits(source, target, start):
  cmd = ['git', 'rev-list', '--oneline', '--reverse', source, f'^{target}']
  if start:
    cmd.append(f'{start}^..{source}')
  return get_out(cmd).splitlines()


def get_change_id_set(branch, exclude_branch, identifier_type):
  if identifier_type == 'pr':
    pattern = r'PR #(\d+)'
  else:
    pattern = r'commit ([0-9a-f]+)'

  change_ids = set()
  cmd = ['git', 'log', '--reverse', '--format=%s', branch, f'^{exclude_branch}']
  subjects = get_out(cmd).splitlines()
  for subject in subjects:
    match = re.match(fr'^(Revert\s+"?)?Cherry pick {pattern}:', subject)
    if match:
      revert, change_id = match.groups()
      if revert:
        change_ids.discard(change_id)
      else:
        change_ids.add(change_id)
  return change_ids


def cherry_pick(sha, num, title):
  log_output = get_out(
      ['git', 'log', '-1', '--format=%ad%x00%an <%ae>%x00%b', sha])
  parts = log_output.split('\x00', 2)
  date = parts[0]
  author = parts[1]
  body = parts[2] if len(parts) > 2 else ''

  msg = f'Cherry pick PR #{num}: {title}\n\n'
  msg += f'Refer to original PR: #{num}\n\n'
  if body:
    msg += f'{body}\n\n'
  msg += f'(cherry picked from commit {sha})'

  cmd = ['git', 'cherry-pick', '--no-commit']
  ps = get_out(['git', 'show', '-s', '--format=%P', sha]).strip().split()
  if len(ps) > 1:
    cmd.append('--mainline=1')
  result = subprocess.run(cmd + [sha], check=True, stdout=sys.stderr)

  if result.returncode != 0:
    status_output = get_out(['git', 'status'])
    files_to_remove = []
    for line in status_output.splitlines():
      if 'deleted by us:' in line:
        filename = line.split('deleted by us:')[1].strip()
        files_to_remove.append(filename)
    if files_to_remove:
      cmd = ['git', 'rm'] + files_to_remove
      subprocess.run(cmd, check=True, stdout=sys.stderr)

  cmd = [
      'git', 'commit', '--no-verify', f'--author={author}', f'--date={date}',
      '-m', msg
  ]
  subprocess.run(cmd, check=True, stdout=sys.stderr)


def main():
  p = argparse.ArgumentParser()
  p.add_argument('--source-branch', required=True)
  p.add_argument('--target-branch', required=True)
  p.add_argument('--start-commit')
  p.add_argument('--max-commits', type=int, default=1000)
  p.add_argument('--identifier-type', default='pr')
  args = p.parse_args()

  # All cherry picked changes on the target branch since the branch point.
  target_change_ids = get_change_id_set(args.target_branch, args.source_branch,
                                        args.identifier_type)
  # All cherry picked changes on the autoroll branch since the branch point.
  autoroll_change_ids = get_change_id_set('HEAD', args.source_branch,
                                          args.identifier_type)
  commits_added = []

  # All commits on the source branch and not on the target branch (all commits
  # since the branch point).
  for line in get_commits(args.source_branch, args.target_branch,
                          args.start_commit):
    if len(commits_added) >= args.max_commits:
      print(f"Reached commit limit ({args.max_commits}).", file=sys.stderr)
      break

    if args.identifier_type == 'pr':
      match = re.match(r'^(\w+) (.*) \(#(\d+)\)$', line)
      if match:
        sha, title, pr_num = match.groups()

        # Skip if the PR is in the skip list.
        if any(
            skip_sha.startswith(sha)
            for skip_sha in _SKIP_LIST.get(args.target_branch, [])):
          continue

        # Skip if the PR is already in the target branch.
        if pr_num in target_change_ids:
          continue

        # Cherry pick if the PR is not already in the autoroll branch.
        if pr_num not in autoroll_change_ids:
          cherry_pick(sha, pr_num, title)
          autoroll_change_ids.add(pr_num)

        commits_added.append(f'- #{pr_num}')
    else:
      match = re.match(r'^(\w+) (.*)$', line)
      if match:
        sha, title = match.groups()

        # Skip if the commit is in the skip list.
        if any(
            skip_sha.startswith(sha)
            for skip_sha in _SKIP_LIST.get(args.target_branch, [])):
          continue

        # Skip if the commit is already in the target branch.
        if any(
            commit_hash.startswith(sha) for commit_hash in target_change_ids):
          continue

        # Cherry pick if the commit is not already in the autoroll branch.
        if not any(
            commit_hash.startswith(sha) for commit_hash in autoroll_change_ids):
          cherry_pick(sha, '', title)
          autoroll_change_ids.add(sha)

        commits_added.append(f'- {sha}')

  if commits_added:
    print('\n'.join(commits_added))


if __name__ == '__main__':
  main()
