# Packaging StageRig

What is in the repository for distributions, and how to submit it.

## The pieces

| File | What it is |
|---|---|
| `scripts/make-source-tarball.sh` | Makes `stagerig-VERSION.tar.xz`: the tree with its JUCE, CLAP and Ableton Link submodules (Link's Asio included), minus JUCE's examples, tools and the AAX/ASIO SDKs and Link's examples. CI attaches one to every release. Distributions build from this, not from GitHub's "Source code" archives, which leave the submodules out. |
| `debian/` | Debian packaging (debhelper 13, cmake + ninja). Builds and passes its tests with `dpkg-buildpackage` from the tarball. |
| `packaging/rpm/stagerig.spec` | Fedora spec, with `Provides: bundled(...)` for what JUCE carries. Fedora has no `clap-devel` yet, so the bundled CLAP headers are used unless built `--with system_clap`. |
| `Resources/io.github.dguedry.StageRig.metainfo.xml` | AppStream metadata for software centres. |
| `docs/StageRig.1`, `docs/stagerig-plugin-host.1` | Man pages. |

CI builds both (`distro-deb`, `distro-rpm` in `.github/workflows/packages.yml`)
from the release tarball on every push.

Build notes for packagers:

- `-DSTAGERIG_SYSTEM_CLAP=ON` (the default) uses the system CLAP headers
  (`libclap-dev`, `clap-devel`) when they are 1.2 or newer, and the bundled
  copy otherwise.
- `-DSTAGERIG_VERSION=...` sets the version shown in the app.
- The tests need a display (`xvfb-run`). The engine test plays Calf
  Monosynth; without an LV2 instrument installed it reports itself skipped
  (exit 77), so Calf need not be a build dependency.
- JUCE 8.0.15 is bundled: it is newer than Debian's `juce-modules-source` and
  not packaged in Fedora. JUCE 8 is AGPLv3 (or commercial); StageRig is
  GPLv3-or-later.

## StageRig's own apt and dnf repository

The `package-repo` job in `.github/workflows/packages.yml` publishes a signed
apt and dnf repository to GitHub Pages after every release, at
<https://dguedry.github.io/stagerig/>. It is rebuilt each time from the last
ten releases' `.deb` and `.rpm` by `scripts/build-package-repo`, which can be
run by hand to check a change (it needs `apt-ftparchive`, `createrepo_c` and
`gpg`). Running the workflow by hand on `main` publishes again without a
release.

- apt: one suite, `stable`, component `main`, `amd64`; `InRelease` and
  `Release.gpg` are signed.
- dnf: `repomd.xml` is signed (`repo_gpgcheck=1`) and carries every package's
  checksum; the packages themselves are not signed (`gpgcheck=0`).
- The key is used for nothing else: RSA 4096, no expiry, fingerprint
  `0584 CCE9 4394 5336 4971  66DD E374 5FD7 E8DE B0F3`. Its private half is
  the `STAGERIG_REPO_SIGNING_KEY` repository secret (armored, no passphrase);
  keep an offline copy. The public half is published as `key.gpg` and
  `key.asc`. A new key means every user fetches it again, so do not rotate it
  lightly.
- Pages is set to deploy from GitHub Actions; the job deploys through the
  `github-pages` environment.

## Other repositories

**Ubuntu PPA (Launchpad).** Create a Launchpad account, upload your GPG key,
create a PPA named `stagerig`. Then, per release and per Ubuntu series:

```sh
V=0.1.57
scripts/make-source-tarball.sh $V ..
cd .. && cp stagerig-$V.tar.xz stagerig_$V.orig.tar.xz && tar -xJf stagerig_$V.orig.tar.xz
cp -r StageRig/debian stagerig-$V/ && cd stagerig-$V
dch -v "$V-1~noble1" -D noble "New upstream release."   # one upload per series
sed -i '/libclap-dev/d' debian/control                    # 24.04 only: no libclap-dev
debuild -S -sa                                           # signed source package
dput ppa:dguedry/stagerig ../stagerig_$V-1~noble1_source.changes
```

Users then `sudo add-apt-repository ppa:dguedry/stagerig && sudo apt install stagerig`.

**Fedora Copr.** Create a Copr project `stagerig` at copr.fedorainfracloud.org,
then either upload the SRPM (`rpmbuild -bs packaging/rpm/stagerig.spec` with the
tarball in `~/rpmbuild/SOURCES`) or point Copr's SCM source at this repository
and the spec path. Users then `sudo dnf copr enable dguedry/stagerig && sudo dnf install stagerig`.

## The official archives

**Debian (and through it, Ubuntu).**

1. File an ITP: `reportbug wnpp`, choose ITP, give the name, licence
   (GPL-3+ with AGPL-3 JUCE), URL and description. Put the bug number in
   `debian/changelog` in place of `#XXXXXX`.
2. Check the package: `lintian -I --pedantic` on the `.changes`, and build in a
   clean chroot (`sbuild` against unstable).
3. Ask the Debian Multimedia Team (salsa.debian.org/multimedia-team) to take
   it under the team; they review audio packages. Or upload to
   mentors.debian.net and file an RFS bug (`reportbug sponsorship-requests`).
4. A sponsor uploads it; it passes the NEW queue, then reaches unstable,
   testing, and Ubuntu's next release by automatic sync.

Expect review questions about the bundled JUCE and `debian/copyright`, which
needs a full licence audit (`licensecheck -r` / `decopy`) before the RFS.

**Fedora.**

1. Get a Fedora account and sign the contributor agreement.
2. File a Package Review request in Red Hat Bugzilla (product Fedora,
   component Package Review) with links to the spec and SRPM, and run
   `fedora-review -b <bug>` yourself first.
3. A sponsor approves you into the packager group; then `fedpkg request-repo`,
   import the SRPM, and build with `fedpkg build`. EPEL is a later branch request.

Before either submission: tag a release whose tarball the watch file and
`Source0` can find, and bump the `<releases>` entry in the metainfo file.
