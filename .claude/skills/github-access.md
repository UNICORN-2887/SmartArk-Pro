# GitHub Access Skill

Access GitHub issues, PRs, discussions, code, and search results using curl + GitHub API and web scraping.

## API Access (authenticated preferred)

### Setup (one-time)
```bash
# Best: use a personal access token (rate limit 5000/hr vs 60/hr)
export GITHUB_TOKEN="ghp_xxxxxxxxxxxx"  # or set via: export GH_TOKEN="..."
```

### Fetch an Issue
```bash
# With token:
curl -s -H "Authorization: Bearer $GITHUB_TOKEN" "https://api.github.com/repos/OWNER/REPO/issues/NUMBER"
# Without token (limited to 60/hr):
curl -s "https://api.github.com/repos/OWNER/REPO/issues/NUMBER"

# Get issue body, title, state from JSON:
curl -s "https://api.github.com/repos/espressif/esp-idf/issues/17889" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(f'Title: {d[\"title\"]}')
print(f'State: {d[\"state\"]}')
print(f'Created: {d[\"created_at\"]}')
print(f'Body:\n{d[\"body\"][:8000]}')
"
```

### Fetch Issue Comments
```bash
curl -s "https://api.github.com/repos/OWNER/REPO/issues/NUMBER/comments?per_page=20" | python3 -c "
import json,sys
comments=json.load(sys.stdin)
for c in comments[:10]:
    print(f'--- {c[\"user\"][\"login\"]} at {c[\"created_at\"]} ---')
    print(c['body'][:2000])
    print()
"
```

### Search Issues/PRs
```bash
QUERY="SDMMC+slot+share+deinit"
curl -s "https://api.github.com/search/issues?q=$QUERY+repo:espressif/esp-idf&per_page=10" | python3 -c "
import json,sys
d=json.load(sys.stdin)
for item in d.get('items',[]):
    print(f'#{item[\"number\"]} [{item[\"state\"]}] {item[\"title\"]}')
    print(f'  {item[\"html_url\"]}')
"
```

### Fetch File Content
```bash
# Get raw file from a specific commit/branch
curl -s "https://raw.githubusercontent.com/OWNER/REPO/BRANCH/path/to/file"
```

### Compare Commits / Versions
```bash
# Compare tags or commits
curl -s "https://api.github.com/repos/espressif/esp-idf/compare/v5.4...v5.5.1" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(f'Files changed: {len(d.get(\"files\",[]))}')
# Show files matching pattern
for f in d.get('files',[]):
    if 'sdmmc' in f['filename'].lower():
        print(f'  {f[\"filename\"]} (+{f[\"additions\"]}/-{f[\"deletions\"]})')
"
```

## Common GitHub URL Patterns

| URL | API Equivalent |
|---|---|
| `github.com/O/R/issues/123` | `/repos/O/R/issues/123` |
| `github.com/O/R/pull/456` | `/repos/O/R/pulls/456` |
| `github.com/O/R/blob/main/x.c` | `https://raw.githubusercontent.com/O/R/main/x.c` |
| `github.com/O/R/compare/A...B` | `/repos/O/R/compare/A...B` |
| Search | `/search/issues?q=...` |

## Usage Instructions

1. When user provides a GitHub URL, extract owner/repo/number
2. Use curl with `python3 -c` to parse JSON and extract relevant info
3. Focus on: title, body, key comments, code snippets, linked PRs, version references
4. Summarize findings in context of the current problem
5. If rate limited, ask user for a GitHub token or use `$GITHUB_TOKEN` env var
