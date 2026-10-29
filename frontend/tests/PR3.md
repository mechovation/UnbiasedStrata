# PR3 — frontend-owned HTTP(S) image acquisition

## Scope

Implemented acquisition before the pinned OpenAI chat parser, for chat and apply-template routes. The parser receives data URLs in the original occurrence order, so it does not download or log remote URLs. Image bytes still use the existing decoder/tokenizer/projector path. No engine, submodule, codec, worker, cache, startup warmup, production Compose, or deployment changes.

Production files: `frontend/acquisition.hpp`, `frontend/acquisition_request.hpp`, PR3 hunks in `frontend/strata_context.cpp` and `CMakeLists.txt`. Build/test documentation: `Makefile`, `README.md`, this file, `acquisition.sh`, `acquisition_test.cpp`, and TLS compile/link flags in the existing white-box `context-lifecycle.sh`. Earlier PR1/PR2 and local deployment edits remain separate; do not stage the entire dirty tree.

## Approved policy

- Installed OpenSSL 3 is required. Configure fails if unavailable/unsupported or if BoringSSL/LibreSSL source-build options are requested. No TLS dependency source downloads or submodule modifications.
- Public HTTP and certificate/hostname-verified HTTPS. Runtime system trust is the default. `STRATA_IMAGE_CA_BUNDLE=/absolute/path/to/ca.pem` selects an explicit CA bundle (include all roots needed by the deployment). Verification cannot be disabled.
- `STRATA_IMAGE_ALLOWED_CIDRS` is a comma-separated list of numeric IPv4/IPv6 CIDRs or single addresses, without whitespace. Example: `192.168.1.0/24,10.20.0.15/32`. Loopback fixtures use `127.0.0.1/32,::1/128`. This grants exceptions for private/loopback/CGNAT/ULA destinations; it is **not** a public-egress allowlist. Public destinations remain enabled.
- Every DNS answer and redirect destination is validated; the connection is pinned to a validated numeric address while retaining the original Host/SNI and certificate hostname. IPv4-mapped IPv6 is normalized. Link-local/metadata (including `100.100.100.200`), multicast, unspecified, reserved IPv4 and selected IPv6 transition ranges cannot be enabled by LAN exceptions. Other unsupported IPv6 scopes are refused.
- Up to five redirects; no HTTPS-to-HTTP downgrade. Credentials in URL authorities are rejected. No inbound Authorization, cookies, or other service headers are forwarded. Errors contain no URL, host, query value or upstream body. Escaped query bytes are preserved for signed URLs.
- Preserve the **10 MiB per remote source** cap. A **10-second wall budget shared by all remote image occurrences in one request** includes DNS, redirects, connect, TLS, and body reads. Deadline/disconnect polling has a 20-ms interval. Slow progress does not renew the budget. Compressed HTTP content encodings are refused; raw image formats are unchanged.
- Acquisition refusals (including certificates, source 4xx, oversized bodies, redirects/egress) return 400; deadline expiry 504; DNS/transport/upstream 5xx failures 502; invalid CIDR configuration 500. These errors occur before SSE headers. Client cancellation terminates acquisition without proceeding to generation.
- Existing inline data/raw-base64 and `--media-path` local-file behavior remain unchanged. No new filesystem access. Per-source caps do not constitute an aggregate decoded-image or request-memory budget.

## Lifetime / boundedness

Pinned httplib's `stop()` cannot interrupt a handshake while its socket mutex is held. Acquisition duplicates the socket descriptor and shuts down that same socket from a joined watcher on cancellation/deadline. The duplicate avoids descriptor-reuse races. The socket-options callback's ownership assumption is documented against this pin; re-audit when upgrading httplib.

Portable libc DNS cannot be forcibly interrupted. One process-wide in-flight lookup is allowed; a busy resolver produces retryable 502 instead of accumulating threads. Timed-out DNS work retains only its hostname/promise, not request callbacks, and cannot lead to a connection after the caller exits. This deliberately favors bounded resources over concurrent DNS throughput; a persistently stuck resolver needs operational recovery. Numeric LAN addresses do not need DNS.

Occurrence coordinates (message index, part index, role) are produced by `acquire_images()` for later role-aware work. Duplicate occurrences and mixed inline/remote images retain parser order; no tool-result omission policy is introduced here.

## Reproduction and results

Dependencies: C++17/20 compiler, CMake, installed OpenSSL 3 development files, `openssl` CLI, Python 3, curl, GNU timeout. No model download, GPU, public image host, or real metadata endpoint is used.

```sh
bash frontend/tests/acquisition.sh
make test
# Also run the PR1/PR2 white-box suite against your CUDA-free static build:
bash frontend/tests/context-lifecycle.sh /path/to/build-frontend
```

Validated in an isolated CPU-only container (8 CPU / 8 GiB limit, no GPU device requests), using an already-local base image. Installed test build dependencies only in that disposable container. The host's missing OpenSSL headers were initially extracted under `/tmp`, not installed system-wide.

- Fresh Release frontend build: pass.
- Adapter (7 cases), literals, picture cache, HTTP/e2e and gated queue regressions: pass.
- Native context lifecycle / media dispatch fixtures: pass.
- Acquisition suite: **26 checks pass**, including trusted/untrusted local CA, hostname mismatch, default-denied/explicit LAN access, mapped IPv6/metadata rules, redirect revalidation/downgrade/count/loop, exact escaped query target, 9-MiB acceptance and fixed/chunked 11-MiB refusal, slow-source/shared request deadlines, disconnect, stalled TLS cancellation, trickling TLS deadline, ordering/bytes/roles and no Authorization forwarding.
- Negative configuration checks: disabling OpenSSL discovery fails; requesting BoringSSL source builds fails before fetching.
- `git diff --check`: pass.

## Authorized 5070 Ti integration smoke

A follow-up GPU smoke test passed using only `GPU-d14daf49-45c8-58c5-cf61-5a40342672af` (RTX 5070 Ti). The candidate frontend was rebuilt with CUDA vision and OpenSSL using the existing pinned CUDA13 build libraries, then mounted into an isolated temporary container using the original production image/native engine/settings. No production image or Compose edits were made.

| Check | Result |
| --- | --- |
| LAN HTTP red image | HTTP 200; correct red answer |
| Local-CA-trusted LAN HTTPS blue image | HTTP 200; correct blue answer |
| Actual projector work | Two fresh GPU encodes confirmed |
| Untrusted certificate / hostname mismatch | Informative HTTP 400 for both |
| Redirect into disallowed loopback / credential-bearing URL | Informative HTTP 400 for both |
| Valid HTTPS vision immediately after each refusal | Four successes, correct blue answers |
| Authentication, text, SSE | Passed on candidate and restored production |
| Credential forwarding / sensitive candidate logs | No service Authorization reached fixtures; no API key or synthetic URL secrets detected in candidate logs |
| Restoration | Original container/image/settings restored; Compose and backup hashes unchanged; two inline vision color checks passed |
| Isolation | Temporary serving/build containers removed; upstream service remained stopped; RTX 3060 untouched |

The successful maintenance supervisor exited 0. Two earlier harness preflight failures (a Python fixture-name collision and an overly strict comparison against Docker's implicit GPU driver field) happened **before production was stopped**; both were corrected and their evidence retained separately. The final run explicitly requested the NVIDIA driver and the sole authorized UUID.

Local operational tooling: `deployment/run-vision-pr3.py` and `deployment/trial-vision-pr3.sh`; local evidence: `logs/vision-pr3-gpu/smoke3/`. These paths are not upstream PR artifacts. Restoration is ownership-scoped and rejects unresolved candidate removal before restarting production; the supervisor ran detached with a bounded trial and an exit restoration path.

This is functional integration evidence, not a GPU performance/quality benchmark or a claim about arbitrary external servers, proxy networks, or native decoder fault containment. **The candidate was not deployed:** production is running its original image. PR4 and later phases remain out of scope.
