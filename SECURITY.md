# Security

gmRunner reads files that come from games: data files, saves, INI and JSON files, buffers,
images and audio. A game, or a file put in its place, could be crafted to make the runner crash
in a way that runs code or reaches outside the game's folders. Problems like that are security
problems.

## Reporting

Please report them privately through GitHub:
[Report a vulnerability](https://github.com/KliffDesaix/gmrunner/security/advisories/new). Don't
open a public issue. Include the gmRunner version, the platform, and how to trigger it; a small
crafted file is fine, but please don't send a game's files.

Reports are answered as soon as possible. Fixes go into the next release, and the advisory is
published with it.

## Supported versions

Only the latest release gets fixes.
