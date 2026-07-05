# Getting Started

## Requirements

- Docker with `buildx`
- Python 3 for command registration and local test tooling
- Terraform for AWS deployment

No local C++ compiler is required. All C++ builds run inside an Amazon Linux
2023 Docker builder image (`docker/lambda-builder/Dockerfile`).

## Build Lambdas

Build one Lambda:

```bash
scripts/build-lambda.sh src/lambdas/discord-interactions
scripts/build-lambda.sh modules/<name>/lambdas/<lambda-folder>
```

Build all framework and installed module Lambdas (run this before Terraform
deploys and before the local integration tests):

```bash
scripts/build-all-lambdas.sh
```

Build for x86_64 instead of the default arm64:

```bash
LAMBDA_ARCH=x86_64 scripts/build-lambda.sh modules/<name>/lambdas/<lambda-folder>
```

Reuse an existing builder image (skip the `docker buildx build` step):

```bash
LAMBDA_SKIP_IMAGE_BUILD=1 scripts/build-lambda.sh modules/<name>/lambdas/<lambda-folder>
```

Zip artifacts are written to `packaged-lambdas/<lambda-name>.zip`.

The build script auto-generates a temporary CMake project from all `*.cpp`,
`*.cc`, and `*.cxx` files found in the Lambda folder. If a Lambda folder
contains its own `CMakeLists.txt`, the build script uses that instead.

### Builder image contents

| Library | Source |
|---|---|
| `aws-lambda-cpp` | Built from source (awslabs/aws-lambda-cpp) |
| `aws-sdk-cpp` (Lambda, DynamoDB, and KMS clients) | Built from source with `BUILD_ONLY="lambda;dynamodb;kms"` |
| `libsodium` | Amazon Linux 2023 DNF |
| `libcurl` | Amazon Linux 2023 DNF |
| `nlohmann/json` | Fetched via CMake `FetchContent` at build time |

`libsodium` is dynamically linked and bundled into every zip alongside
`bootstrap`.

## Run the tests

The fast C++ unit tests need no zip build or emulator — just Docker:

```bash
scripts/test-unit.sh                       # run all C++ unit tests
scripts/test-unit.sh --filter 'route*'     # run a subset by doctest filter
```

The Python unit layer needs no Docker at all:

```bash
python3 -m pytest tests/unit/python
```

The full local integration suites run the built zips inside the AWS Lambda
Runtime Interface Emulator:

```bash
scripts/build-all-lambdas.sh
scripts/test-local-discord-lambdas.sh
```

See [Testing](testing.md) for the suite catalogue, fixtures, and the mock
server API.

## Next steps

- [Register your commands with Discord](command-registration.md)
- [Deploy the stack with Terraform](deployment.md)
- [Scaffold your first module Lambda](modules.md#scaffolding-a-new-lambda)
