# Contributing

Small, reproducible improvements and honest compatibility reports are welcome.
This project is maintained independently of BeyondATC and Laminar Research;
there is no support SLA or promise of compatibility with future app updates.

Before a report, read [troubleshooting](docs/troubleshooting.md). Include:

- Repository commit, distribution, CPU architecture and display/session setup.
- X-Plane, Wine and BeyondATC version/channel.
- Relevant aircraft, CSL model and scenery/airport.
- Steps, expected behavior, observed behavior and approximate UTC time.
- A minimal, redacted diagnostic excerpt if available, identifying command,
  transport and rendering behavior separately.

**Do not attach** purchased software/models, vendor source or disassembly,
modified assemblies, authenticated Wine prefixes, tokens, audio/transcripts,
whole vendor logs or unreviewed flight recordings. Use synthetic reproductions
where possible. Do not represent a Wine issue as a supported-Windows test.

For changes, explain the behavior being fixed and add a focused regression.
Run the relevant checks in [Development](docs/development.md), including a full
package build for native/companion changes. Describe runtime testing separately
from isolated tests. Never test an installer against a running flight.

Contributions to original project code are under the repository's MIT license.
Keep upstream attribution and do not submit material you cannot license.

For a suspected sensitive issue, do not post credentials or exploitable details
in a public issue; use GitHub's private vulnerability reporting if enabled.
