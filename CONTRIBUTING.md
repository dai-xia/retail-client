# Contributing to RetailSystem_RK3568

Thanks for your interest in this project! This document describes how to
contribute.

## Repository Layout

```
common/   Shared C library (crypto, logger, cJSON, common types)
server/   Qt + C network framework server (epoll / io_uring + thread pool)
client/   Qt client (face pay, voice, hardware, OTA, video streaming)
driver/   Linux kernel drivers (RC522, BH1750, LED, motor) for RK3568
deploy/   systemd service and deployment scripts
3rdparty/ Vendored libraries (liburing headers)
```

## Build From Source

See `README.md` for build dependencies and instructions.

```bash
# Server
cd server && qmake RetailServer.pro && make -j$(nproc)

# Client (x86 build for development)
cd client && qmake RetailClient.pro && make -j$(nproc)
```

## Before Submitting a Pull Request

1. **No hardcoded absolute paths.** Build configuration must be portable
   (use environment variables or `find_package`).
2. **No Chinese comments.** All new code must use English comments so the
   codebase stays consistent and accessible to international contributors.
3. **No secrets.** Never commit private keys, certificates, real passwords,
   or vendor SDK paths. Sensitive files are covered by `.gitignore`.
4. **No build artifacts.** Make sure `*.o`, `*.ko`, `build/`, `__pycache__/`
   etc. are not added to your commit.
5. **Keep the style consistent.** See `.editorconfig` for indentation and
   newline rules.

## Coding Conventions

- C/C++ source uses 4-space indentation, LF line endings, no trailing
  whitespace.
- C file headers: brief module description + SPDX-style tag if applicable.
- Public API goes in `*.h` with `#ifndef ... #define ... #endif` guards.
- Logging goes through the `logger` module — never `printf` in library code.
- Hardware paths: DMA-BUF fd for hardware (RGA / RKNN / VPU), `mmap` virtual
  address only for CPU access.
- Video streaming: PTS is captured at the capture moment and assigned to
  `AVFrame->pts`; never overwrite `enc_pkt->pts` returned by the encoder.

## Pull Request Process

1. Fork the repository and create a feature branch from `main`.
2. Make focused commits with clear messages (English, imperative mood).
3. Update `README.md` if your change affects the public API, protocol, or
   build flow.
4. Open a pull request describing **what** changed and **why**, referencing
   any related issue.

## Reporting Issues

When opening an issue, please include:

- Hardware / OS / toolchain version (e.g. RK3568, Ubuntu 20.04, gcc-linaro).
- Steps to reproduce.
- Relevant log snippet from `/var/log/retail/`.
- Expected vs actual behavior.

## License

By contributing, you agree that your contributions will be licensed under the
MIT License, as described in `LICENSE`.
