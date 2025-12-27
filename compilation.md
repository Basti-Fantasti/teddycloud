# TeddyCloud Compilation Guide

This document describes how to build TeddyCloud using the Docker-based build environment.

## Prerequisites

- Docker Desktop installed and running
- Git (with submodule support)

## Initial Setup

Initialize all submodules before building:

```bash
git submodule update --init --recursive
```

## Building with Docker

The project includes a Docker build environment in `.devcontainer/Dockerfile`.

### Build the Docker Image (one-time)

```bash
docker build -t teddycloud-build -f .devcontainer/Dockerfile .
```

### Compile the Project

**Linux/Mac:**
```bash
docker run --rm -v "$(pwd):/buildenv" -w /buildenv teddycloud-build make build
```

**Windows (Git Bash/MSYS2):**
```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "X:/path/to/teddycloud:/buildenv" -w /buildenv teddycloud-build make build
```

Replace `X:/path/to/teddycloud` with your actual project path.

### Other Build Targets

```bash
# Full build (backend + frontend)
docker run --rm -v "/path/to/teddycloud:/buildenv" -w /buildenv teddycloud-build make all

# Clean build artifacts
docker run --rm -v "/path/to/teddycloud:/buildenv" -w /buildenv teddycloud-build make clean

# Create release package
docker run --rm -v "/path/to/teddycloud:/buildenv" -w /buildenv teddycloud-build make preinstall zip
```

## Output

The compiled binary is located at `bin/teddycloud`.

## Notes

- The Docker image uses Ubuntu with GCC, protobuf-c-compiler, and other build tools
- Compiler warnings are treated as errors (`-Werror`)
- Linux builds include AddressSanitizer and UBSan for debugging
