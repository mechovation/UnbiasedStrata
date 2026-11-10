# Model restriction

- Astra is prohibited for all work on this project, including delegated agents.
  Verify the selected model before working or delegating; never select Astra.

# Deployment rules

- Production moved on 2026-11-05: the sole authoritative deployment is
  `/home/eric/Docker/Stack/docker-compose.yaml`, service/container `strata-5070ti`.
  Its single backup is `docker-compose.yaml.bak` in Stack. No Compose overrides
  or stacked deployment files. Update production settings directly in Stack.
  This repository's Compose service is retired (profile `retired`, restart `no`);
  never enable it alongside production.
- GPU allocation, rescoped 2026-11-07:
  * Radeon AI PRO R9700 (gfx1201) is the designated GPU for development GPU work:
    HIP/ROCm baselines, builds, probes, smoke tests, and Vulkan experiments. Select
    only its AMD render node and `/dev/kfd` in a dedicated development environment;
    never expose all GPUs to a dev container. The RTX 3060 is no longer on this server.
  * RTX 5070 Ti (GPU-d14daf49-45c8-58c5-cf61-5a40342672af) is reserved for the running
    production service and for final NVIDIA qualification. Request permission before
    testing on the RTX 5070 Ti, and do so only while GPT controls the session.
  * Self-host dependency: One of the coding agent's models (`qwen3.8-flash-next`) is served
    by production `strata-5070ti` in `~/Docker/Stack` on the 5070 Ti. An agent session must not stop, restart,
    or oversubscribe that service unless GPT is the model being used in the session: doing so
    while using qwen3.8-flash-next removes the reasoning model mid-task.
    Qwen should hand 5070 Ti work over to GPT (see `deployment/PR7-5070TI-RUNBOOK.md`).
  * Architecture: the R9700 is gfx1201 (HIP: `CMAKE_HIP_ARCHITECTURES=gfx1201`);
    the 5070 Ti is sm_120 (CUDA). R9700 results are not 5070 Ti qualification evidence.
- Pull large Docker base images independently in the background before building.
  Do not use a build to initiate large image downloads. Use the existing hf-download
  container for any needed background Hugging Face downloads.
- Keep API keys out of source, images, logs, and command-line arguments.

# Development environment

- `deployment/dev-container.sh` and `deployment/Dockerfile.dev` are CUDA/RTX 3060-era
  tooling, not a HIP/R9700 environment. Do not create or run the old `gpu-build` path;
  replace it before development GPU builds or tests. No second Compose file.
- Development builds/tests belong in a dedicated, bounded environment with the R9700
  explicitly selected (not `--gpus all`), a clean checkout without `src/secrets`
  mounted read-only, and build artifacts outside the source tree. Edit and commit on the host.
  Preserve the 12 CPU / 32 GiB build caps so development cannot starve production.
  A separate R9700-only inference container may use 12 CPUs / 96 GiB for full-model
  baselines on this 495 GiB host; never run builds in that container. Monitor
  production health and host headroom during inference.
- Never attach the 5070 Ti to the development environment. Final NVIDIA qualification
  requires the separate permission and GPT-only conditions above.

# Reply length

- Keep chat replies under ~100 words. Put findings, evidence and citations in files
  and name the path. No tables or long quotes in chat.
