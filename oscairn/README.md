# setBfree for Oscairn

setBfree's tonewheel organ (tone generator, vibrato scanner, percussion, overdrive, reverb
and Leslie) as a CLAP instrument for [Oscairn](https://github.com/oscairn/oscairn), with
Oscairn's native sound contexts. The plan, the integration checklist and what differs from
upstream setBfree are in [doc/oscairn-plan.md](../doc/oscairn-plan.md).

```sh
cmake -S oscairn -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
oscairn/tools/compare-stock.sh build/gate   # the stock-vs-fork gate
```

`build/setbfree.clap` holds two plugins: "setBfree" (`org.oscairn.setbfree`), the organ, and
"setBfree Leslie" (`org.oscairn.setbfree.leslie`), its rotary speaker as an audio effect with
Oscairn's native effect contexts.

## Releasing

The package version is the CMake project version in `oscairn/CMakeLists.txt`; each plugin's own
version lives in its descriptor (`oscairn/plugin.cpp`, `oscairn/leslie.cpp`; the release scripts
read them there). Bump what changed, commit, then tag the package version and push the tag:

```
git tag oscairn-v0.4.5
git push origin oscairn-v0.4.5
```

The [release workflow](../.github/workflows/release.yml) builds the `.clap` for each platform,
runs the tests and the device dependency checks, and creates a GitHub Release holding
`oscairn-setbfree-<platform>.clap`, `licenses.tar.gz` and `oscairn-release.json`. The tag version
must match the CMake project version; the release fails if it does not. To try the workflow
without a tag, run it manually (`workflow_dispatch`) with *dry run* checked: everything runs and
the files land as a workflow artifact instead of a release. The repo-specific facts (names, ids,
plugins, licence) live in `oscairn/tools/release/release.conf`.

## Credits and licence

setBfree is by Fredrik Kilander, Will Panther, Robin Gareus and Ken Restivo (see `AUTHORS`),
under the GNU General Public License, version 2 or (at your option) any later version
(`COPYING`). The files in this folder are under the same licence. `third_party/clap` (the
CLAP headers) and `third_party/oscairn-clap` (Oscairn's plugin SDK headers) are MIT; each
folder's `upstream.json` records the commit it was copied from, unchanged.
