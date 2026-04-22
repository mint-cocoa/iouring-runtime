# Security Policy

`iouring-runtime` is still early-stage software and has not yet gone through a formal security review.

## Supported Scope

Security-sensitive areas include:

- socket and session lifecycle handling
- timeout and forced-close behavior
- queue growth and backpressure limits
- shutdown and drain semantics
- unsafe memory or ownership bugs in the runtime core

## Reporting

If you discover a security issue, please avoid posting exploit details publicly right away.

For now, open a GitHub issue that requests a private follow-up and include only the minimum information needed to reproduce the problem safely. Once a dedicated private reporting channel is added, this file should be updated to point to it directly.

## Expectations

- Provide affected component names when possible
- Include build type, kernel, and `liburing` version if relevant
- Minimize proof-of-concept impact when sharing repro steps
