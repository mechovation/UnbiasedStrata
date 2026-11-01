# Model restriction

- Astra is prohibited for all work on this project, including delegated agents.
  Verify the selected model before working or delegating; never select Astra.

# Deployment rules

- HARD RULE: exactly one authoritative `docker-compose.yaml` and one backup,
  `docker-compose.yaml.bak`. No Compose overrides, alternate Compose YAML files,
  or settings that require stacking deployment files. Update production settings
  directly in `docker-compose.yaml`.
- Use only the RTX 5070 Ti (GPU-d14daf49-45c8-58c5-cf61-5a40342672af).
  Do not use or disrupt the RTX 3060.
- Pull large Docker base images independently in the background before building.
  Do not use a build to initiate large image downloads. Use the existing hf-download
  container for any needed background Hugging Face downloads.
- Keep API keys out of source, images, logs, and command-line arguments.

# Reply length

- Keep chat replies under ~100 words. Put findings, evidence and citations in files
  and name the path. No tables or long quotes in chat.
