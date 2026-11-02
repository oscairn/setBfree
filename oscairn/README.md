# setBfree for Oscairn

setBfree's tonewheel organ (tone generator, vibrato scanner, percussion, overdrive, reverb
and Leslie) as a CLAP instrument for [Oscairn](https://github.com/jbschooley/oscairn), with
Oscairn's native sound contexts. The plan, the integration checklist and what differs from
upstream setBfree are in [doc/oscairn-plan.md](../doc/oscairn-plan.md).

```sh
cmake -S oscairn -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
oscairn/tools/compare-stock.sh build/gate   # the stock-vs-fork gate
```

`build/setbfree.clap` is the plugin: "setBfree", `org.oscairn.setbfree`.

## Credits and licence

setBfree is by Fredrik Kilander, Will Panther, Robin Gareus and Ken Restivo (see `AUTHORS`),
under the GNU General Public License, version 2 or (at your option) any later version
(`COPYING`). The files in this folder are under the same licence. `third_party/clap` (the
CLAP headers) and `third_party/oscairn-clap` (Oscairn's plugin SDK headers) are MIT; each
folder's `upstream.json` records the commit it was copied from, unchanged.
