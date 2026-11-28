# SleepyGram stable releases

SleepyGram checks GitHub Latest for a stable release. Version tags use
`vMAJOR.MINOR.PATCH`, where the matching app version is defined in
`Telegram/build/version` and `Telegram/SourceFiles/core/version.h`. The release
has three signed update assets alongside the manual downloads.

| Platform | Update asset | Manual downloads |
| --- | --- | --- |
| Linux x64 | `SleepyGram-update-linux-x64.tdup` | `SleepyGram-linux-x64.tar.xz` |
| macOS arm64 | `SleepyGram-update-macos-arm64.tdup` | `SleepyGram-macos-arm64.dmg`, `SleepyGram-macos-arm64.zip` |
| Windows x64 | `SleepyGram-update-windows-x64.tdup` | `SleepyGram-windows-x64.zip` |

Install the first build that embeds the production trust root using its manual
download. Later releases signed for the same trust root can update it in-app.
Builds made with the repository's public test keys cannot use the production
feed. The workflow rejects these keys for publication.

## Configure GitHub once

Protect the `dev` branch with reviewed changes. Create the `sleepygram-release`
environment with deployment limited to `dev` and required approval from trusted
release reviewers. The reviewer should inspect the exact dispatched commit and
its three platform jobs before approving the job. Enable immutable releases in
the repository so published tags and assets are locked by GitHub. If a tag
ruleset restricts `v*` tag creation, provision the release tag on the exact
commit before dispatch. [GitHub environments](https://docs.github.com/en/actions/how-tos/deploy/configure-and-manage-deployments/manage-environments) · [Immutable releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases#immutability)

## Generate the production trust bundle

Use OpenSSL 3 or newer in a private directory outside the checkout. Keep the
root private key offline with a backup. The release job needs only the separate
stable signing key.

```bash
umask 077
openssl genpkey -algorithm Ed25519 -out root-private.pem
openssl pkey -in root-private.pem -pubout -out root-public.pem
openssl genpkey -algorithm Ed25519 -out stable-private.pem
openssl pkey -in stable-private.pem -pubout -outform DER -out stable-public.der
python3 - <<'PY'
import base64
import json
from pathlib import Path

prefix = bytes.fromhex("302a300506032b6570032100")
public = Path("stable-public.der").read_bytes()
assert public.startswith(prefix) and len(public) == len(prefix) + 32
x = base64.urlsafe_b64encode(public[len(prefix):]).rstrip(b"=").decode("ascii")
manifest = {
    "format": 1,
    "manifest_version": 1,
    "keys": [{"id": "sleepygram-stable-1", "alg": "Ed25519", "x": x}],
    "channels": {"stable": [["sleepygram-stable-1"]]},
    "revoked": [],
}
Path("manifest.min.json").write_bytes(json.dumps(manifest, separators=(",", ":")).encode("ascii"))
PY
openssl pkeyutl -sign -rawin -inkey root-private.pem -in manifest.min.json -out manifest.sig
openssl pkeyutl -verify -rawin -pubin -inkey root-public.pem -in manifest.min.json -sigfile manifest.sig
python3 - <<'PY' > public-bundle.json
import base64
import json
from pathlib import Path

files = ("root-public.pem", "manifest.min.json", "manifest.sig")
print(json.dumps({name: base64.b64encode(Path(name).read_bytes()).decode("ascii") for name in files},
                 separators=(",", ":")))
PY
```

Save the JSON as a repository Actions variable and the stable private PEM as
an Actions secret scoped to the protected environment. Use the root only to sign
new trust manifests. When rotating a stable key, add the new public key to the
same stable group and increase `manifest_version` before signing with the root.
Keep the root public key stable across versions to preserve the update path.

```bash
gh variable set --repo slpkbt/SleepyGram SLEEPYGRAM_UPDATE_PUBLIC_BUNDLE < public-bundle.json
gh secret set --repo slpkbt/SleepyGram --env sleepygram-release SLEEPYGRAM_UPDATE_STABLE_KEY_PEM < stable-private.pem
```

## Publish the release

Dispatch **Build SleepyGram** from protected `dev` with `platform=all` and
`publish_release=true`. Choose a new app version greater than every published
stable version. The workflow reads the tag from the checked-out version and
requires all three native packages from that commit and workflow attempt to
pass their startup smokes. The release job checks the unsigned TDUP bytes
against the manual archives, signs them with the stable key and verifies them
with the same verifier as the client. It attaches the files to a draft, checks
each uploaded digest and then publishes as Latest.

If package verification, signing, or draft upload fails, the previous release stays Latest.
An interrupted upload can leave an unpublished draft. Inspect that draft before
retrying. The workflow refuses to reuse its tag until the incomplete draft has
been removed, and it always refuses to overwrite a published release.

## macOS distribution policy

SleepyGram's TDUP signature authenticates update bytes but is not a substitute
for Apple's Developer ID signature or notarization. macOS builds that include
the preflight assess the verified replacement bundle before requesting a
restart. A rejection leaves the running application in place and shows an
explanation in Version and Updates next to the link to SleepyGram Releases.
That link does not override macOS policy and the manual package may be subject
to the same policy. Older installed builds do not have this preflight.

For production macOS auto-updates, sign the nested code and the final app with
Developer ID Application and the required hardened runtime and entitlements,
submit it to Apple's notary service, and test the exact app reconstructed
from the signed TDUP. Assess that reconstructed app on a clean Mac, including
offline and in a normal Finder-launched upgrade. The current TDUP comparison
checks file bytes and executable bits, not AppleDouble metadata or extended
attributes; a passing DMG check alone is not sufficient.
[Apple notarization guide](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
