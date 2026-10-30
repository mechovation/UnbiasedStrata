# PR6 candidate library notices

These unmodified OSS libraries are used by the PR6 native reader and its C++
normalizer tests. The frontend also links them for configuration identity, not
image decoding. The development build targets now depend on them, but no
production image has been updated or approved for distribution. An offline
candidate bundle now packages and tests the codec-specific obligations; see
[checkpoint status](../../docs/PR6_STATUS.md). Unrestricted release approval
is not implied. No library implementation is vendored here; this directory contains
attribution, verbatim license records and artifact hashes.

The application remains MIT. That does not relicense the libraries or remove the
following obligations. Exact file-level terms and copyright holders are retained
in `licenses/`; those records govern over this summary.

| Component / evaluated package version | Terms and purpose |
| --- | --- |
| libwebp, demux and mux / 1.3.2-0.4build3 | BSD-3-Clause and Google's additional patent grant; retain copyright, license conditions, disclaimer and applicable patent terms; no endorsement using contributor names. Decoder/demux are runtime dependencies of the new build; mux is fixture-only. |
| libsharpyuv / 1.3.2-0.4build3 | WebP package's BSD-3-Clause/patent terms; an actual shared dependency of the evaluated WebP decoder binary, not an optional dependency to omit from notices. |
| libyuv / 0.0~git202401110.af6ac82-1 | BSD-3-Clause and Google's additional patent grant, reproduced in its copyright record; public rotation/mirroring/channel-conversion/compositing APIs only. |
| libexif / 0.6.24-1ubuntu0.24.04.1 | Library code is LGPL-2.1-or-later with some LGPL-2.0-or-later files; LGPL 2.1 is the common permitted version for this evaluation. Preserve full notices and LGPL text. Link as a replaceable shared library, not a static archive. Packaging/source-file licenses, including GPL packaging scripts and separately licensed translations, are distinguished in the verbatim copyright record; those scripts are not linked into the application. |
| libjpeg-turbo / 2.1.5-2ubuntu2 | IJG/BSD-style, BSD-3-Clause and zlib code terms; preserve upstream notices and the required IJG acknowledgment below. This is an actual shared dependency of the evaluated libyuv binary. JPEG writing in the proof uses its public fixture-generation API, not a new application JPEG decoder. |

**This software is based in part on the work of the Independent JPEG Group.**

## LGPL distribution plan (required before shipping)

The new binaries use a replaceable `libexif.so.12` through the ordinary shared loader;
`readelf` confirms a shared dependency, not embedded library object code. MIT
application terms allow modification and reverse engineering; add no conflicting
restriction. Preserve users' ability to replace the shared library with an
interface-compatible version, including in containers/custom runtime profiles.
Do not impose integrity checks that forbid user-built replacement libraries;
output-affecting library identity may change the cache namespace instead.

Any release that redistributes libexif must include the applicable notices and
license and provide its complete corresponding source under the applicable LGPL
terms. A general upstream URL is not a substitute. The evaluated Ubuntu library's
matching source is the original tarball **plus the exact Debian/Ubuntu source
packaging/build scripts and published changes**, identified by its `.dsc` and
hashes in `ARTIFACTS.sha256`. These were downloaded via authenticated Ubuntu source
metadata into `/tmp/ub-pr6-deps/source/`, without executing build scripts or
altering the library. Preserve source notices and applicable build-script licenses.

Package those matching source archives alongside the binaries in release artifacts
(or provide compliant equivalent access from the same distribution location),
plus the full license/notice directory. Do not rely on an unimplemented written
offer. Test notices, source completeness, shared linkage and replacement support
in actual release/container packaging before claiming distribution compliance.
No production image or release package was changed during this development work.

See the single published [PR6 status summary](../../docs/PR6_STATUS.md) for
implemented behavior, qualification and remaining conditions. Detailed PR5/PR6
working documents and tests are intentionally not included in this checkpoint.
Real-engine/GPU and candidate codec-compliance gates passed for the exact tested
policy; broader omission coverage, security/support caveats and final shipping-
image verification remain open. Unrestricted release approval is not implied.
