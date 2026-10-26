# BeyondATC on Linux with native X-Plane

An **unofficial, experimental** connector between native Linux X-Plane and the
Windows BeyondATC Experimental application running under Wine.

The simulator stays native. A native plugin renders traffic and reports aircraft
state; a small Wine companion provides simulator discovery and push-to-talk.
BeyondATC still handles speech, flight planning and traffic decisions.

**This is not a Linux release of BeyondATC, not vendor-supported, and not a
replacement for purchasing the application.** You must obtain BeyondATC, its
Windows X-Plane plugin, X-Plane and traffic models separately, under their own
licenses. No vendor binaries, account data, app patches or traffic models are
included.

## Status and scope

The integration has been used for complete flights with voice/PTT, clearance,
controller handoffs, radio tuning and traffic injection. The reference environment
is **x86_64 Linux, NixOS, Wine, X-Plane 12.4.4 and BeyondATC
1.11.0.Experimental (protocol 4)**. Other distributions with Nix and a systemd user
session are a portability target, not a tested compatibility claim.

Treat this as a project for technically comfortable early adopters. In particular:

- The interface is reverse-engineered and unversioned. Vendor updates can break it.
- The reference installation also used private local repairs for two
  Experimental-app runway crashes. **Those repairs are not in this project**;
  an unmodified application may still encounter those failures.
- The AGL-feedback correction and near-ground terrain floor are included. Replay
  tests cover recorded landing failures, but visual touchdown quality is not
  established across aircraft/models/scenery.
- Traffic may be commanded to stop in inappropriate places, including a recorded
  aircraft stationary on a runway. This connector does not implement an
  independent traffic controller or guarantee separation.
- Traffic jetways, precise nose-wheel/model feedback and full Windows-plugin
  parity are not implemented. Automatic transponder writes are not independently
  confirmed.

For simulator use only. Never use it for real-world navigation or traffic
avoidance.

## Getting started

The supported packaging path uses **Nix with flakes**, including on non-NixOS
distributions. It builds the native plugin and Windows companion from source and
provides a managed installer, launcher and systemd user service.

```sh
git clone https://github.com/keskvarc/beyondatc-linux.git
cd beyondatc-linux
nix build
```

**Do not copy the plugin into a running simulator.** First read
[Installation](docs/installation.md): it covers the official app/private Wine
prefix, model acquisition, explicit disabling of competing injectors, configuration
and service setup. `nix build` alone does not install anything into X-Plane.

Once configured:

```sh
systemctl --user start beyondatc-native.service
beyondatc-native launch
```

Assign X-Plane's **BeyondATC: Push to Talk (hold)**
(`beyondatc/push_to_talk`) to a key or joystick button. Keep BeyondATC's own PTT
binding set to **Down Arrow**.

## Documentation

| Document | Contents |
|---|---|
| [Installation and recovery](docs/installation.md) | Prerequisites, Wine prefix, models, configuration, updates and removal |
| [Troubleshooting and diagnostics](docs/troubleshooting.md) | Connection, PTT, missing/incorrect traffic, bounded incident recording |
| [Architecture](docs/architecture.md) | Native/Wine boundary, protocol, coordinate units and safety limits |
| [Development](docs/development.md) | Source layout, build/test commands and regression fixtures |
| [Contributing](CONTRIBUTING.md) | Useful reports, privacy and contribution expectations |
| [Third-party notices](THIRD_PARTY_NOTICES.md) | Dependency attribution and licensing boundaries |

## License and independence

Original project code is [MIT licensed](LICENSE). XPMP2, the X-Plane SDK, Wine,
compiler runtimes and other dependencies retain their own licenses. BeyondATC,
X-Plane and the relevant model libraries remain their owners' products and
trademarks. No affiliation, endorsement or official support is implied.
