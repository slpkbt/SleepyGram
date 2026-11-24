# CI package checks

On build failure, `build_diagnostics.py` adds the first compiler or linker
errors to the job's check annotations. Read them in the job's Annotations
section or with `gh api repos/slpkbt/SleepyGram/check-runs/JOB_ID/annotations`.

The platform jobs package with `tar`, `Compress-Archive`, and `ditto` and use
`build_artifact.py` to write or verify a safe `build-manifest.json` in each
archive. The manifest records the source commit,
workflow run, actual recursive submodule commits, Qt version, runner, and
compiler/toolchain versions. It contains no credentials or environment dump.

The package check validates archive paths and required bundle files, then reads
the executable headers to confirm Linux x64, Windows x64, or macOS arm64. The
Windows archive retains the x64 `d3dcompiler_47.dll` module when the build
produces it for the Windows 7 baseline. The packages include the platform update helper.

The startup smoke launches the extracted package for at most eight seconds with
`-workdir` pointed at a temporary profile and temporary home/config directories.
Linux's Qt build enables xcb and Wayland, not the offscreen plugin, so CI starts
host Xvfb and runs xcb inside the build container with networking disabled.
The platform jobs require the process to stay up for the full window before
uploading a package. The startup check uses a disposable profile without
signing in or exercising account features.

Each platform prepares an unsigned TDUP from the files in its manual archive.
Release publishing requires a manual dispatch with all three platforms on
protected `dev` and approval in the release environment. See the
[stable release and key setup guide](../../docs/signed-updates.md).

## Build modes

Pushes to `dev` and PRs targeting `dev` always run source preflight. Shared
client source changes build Linux. Platform-specific source changes also
select their platform. Shared headers, build configuration, submodules, and
CI changes select all three platforms. Documentation-only changes skip the
platform jobs. The Windows SDK version in `docs/building-win.md` selects Windows.

Manual runs build and upload the selected packages by default. Select `all`
with `publish_release=false` to also verify that the three packages and
unsigned updates came from this run. Select `ci_only=true` to run the platform
checks without uploading successful packages. Fast CI stops Ninja at the
first failure and accepts `build_jobs` values of 2, 3, or 4.

Linux fast CI mounts a 1,500 MB ccache in the build container and uses the
public API defaults. The build step reports compiler hits and misses, cache
footprint, and executable link time. PRs restore existing caches. Pushes to
`dev` and manual fast runs can prime a weekly compiler cache scoped to their
ref. Manual package builds and stable releases use the original precompiled
header and never restore the compiler cache. The dependency caches are saved
on `dev`. Manual runs can opt in to dependency saves for their own ref with
`save_dependency_cache=true`. macOS restores or builds its dependencies in the
same job as the client and saves them after the existing build-intermediate
cleanup.
