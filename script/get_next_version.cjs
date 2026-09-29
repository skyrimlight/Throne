#!/usr/bin/env node

/**
 * Automatically determine the next incremented version tag based on existing
 * Git tags and GitHub Releases.
 * 
 * Usage:
 *   node script/get_next_version.cjs [branch] [manualTag] [repo]
 */

const https = require('https');
const { execSync } = require('child_process');
const fs = require('fs');

const branch = process.argv[2] || process.env.BRANCH || process.env.GITHUB_REF_NAME || 'dev';
const manualTag = process.argv[3] || process.env.MANUAL_TAG || process.env.INPUT_TAG || '';
const repo = process.argv[4] || process.env.GITHUB_REPOSITORY || 'skyrimlight/Throne';
const token = process.env.GITHUB_TOKEN || process.env.DEFAULT_TOKEN || '';

function fetchJson(path) {
  return new Promise((resolve) => {
    const options = {
      hostname: 'api.github.com',
      path,
      headers: {
        'User-Agent': 'Node.js',
        ...(token ? { 'Authorization': `token ${token}` } : {})
      },
      timeout: 10000
    };
    const req = https.get(options, (res) => {
      let data = '';
      res.on('data', chunk => data += chunk);
      res.on('end', () => {
        try {
          resolve(JSON.parse(data));
        } catch {
          resolve([]);
        }
      });
    });
    req.on('error', () => resolve([]));
    req.on('timeout', () => { req.destroy(); resolve([]); });
  });
}

async function resolveNextVersion() {
  // If an explicit manual tag is provided, use it directly
  if (manualTag && manualTag.trim() && manualTag.trim() !== 'v1.0.8-dev' && manualTag.trim() !== 'auto' && manualTag.trim() !== 'r') {
    return {
      tag: manualTag.trim(),
      isManual: true,
      previousTag: 'manual'
    };
  }

  const allTagNames = new Set();

  // 1. Gather local git tags
  try {
    const localTags = execSync('git tag -l', { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
    localTags.split('\n').map(t => t.trim()).filter(Boolean).forEach(t => allTagNames.add(t));
  } catch {}

  // 2. Gather remote GitHub releases
  try {
    const releases = await fetchJson(`/repos/${repo}/releases?per_page=50`);
    if (Array.isArray(releases)) {
      releases.forEach(r => {
        if (r.tag_name) allTagNames.add(r.tag_name);
      });
    }
  } catch {}

  // 3. Gather remote GitHub tags
  try {
    const tags = await fetchJson(`/repos/${repo}/tags?per_page=50`);
    if (Array.isArray(tags)) {
      tags.forEach(t => {
        if (t.name) allTagNames.add(t.name);
      });
    }
  } catch {}

  // Defaults: baseline is 1.0.8
  let maxMajor = 1;
  let maxMinor = 0;
  let maxPatch = 8;
  let hasFoundMatch = false;

  for (const rawTag of allTagNames) {
    const m = rawTag.match(/^v?(\d+)\.(\d+)\.(\d+)/);
    if (m) {
      hasFoundMatch = true;
      const major = parseInt(m[1], 10);
      const minor = parseInt(m[2], 10);
      const patch = parseInt(m[3], 10);

      if (
        major > maxMajor ||
        (major === maxMajor && minor > maxMinor) ||
        (major === maxMajor && minor === maxMinor && patch > maxPatch)
      ) {
        maxMajor = major;
        maxMinor = minor;
        maxPatch = patch;
      }
    }
  }

  const previousTag = `v${maxMajor}.${maxMinor}.${maxPatch}${branch === 'dev' ? '-dev' : ''}`;
  const nextPatch = maxPatch + 1;
  const isDev = branch === 'dev' || branch.includes('dev');
  const nextTag = `v${maxMajor}.${maxMinor}.${nextPatch}${isDev ? '-dev' : ''}`;
  const versionNum = `${maxMajor}.${maxMinor}.${nextPatch}`;

  return {
    tag: nextTag,
    version: versionNum,
    previousTag,
    isManual: false
  };
}

resolveNextVersion().then((res) => {
  // If in GitHub Actions, write outputs to $GITHUB_OUTPUT
  if (process.env.GITHUB_OUTPUT) {
    fs.appendFileSync(process.env.GITHUB_OUTPUT, `tag=${res.tag}\n`);
    fs.appendFileSync(process.env.GITHUB_OUTPUT, `version=${res.version || res.tag.replace(/^v/, '')}\n`);
    fs.appendFileSync(process.env.GITHUB_OUTPUT, `previous_tag=${res.previousTag}\n`);
  }

  // Print tag to stdout
  console.log(res.tag);
}).catch((err) => {
  console.error('[Version Resolver Error]', err);
  const fallback = 'v1.0.9-dev';
  if (process.env.GITHUB_OUTPUT) {
    fs.appendFileSync(process.env.GITHUB_OUTPUT, `tag=${fallback}\n`);
  }
  console.log(fallback);
});
