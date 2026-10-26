# Third-party notices

The MIT license at the repository root covers this project's original code,
not other vendors' applications, models, SDKs or trademarks.

## XPMP2

Native traffic rendering uses [TwinFan/XPMP2](https://github.com/TwinFan/XPMP2),
pinned to `fb543f9d063e7fc61e08f9d21c6e68136606fc23`.
Its source/resources are fetched as a build dependency, not copied into this
repository. Installed packages include `share/beyondatc-native/LICENSE.XPMP2`.

XPMP2 is MIT licensed, Copyright (c) 2020 B. Hoppe. Its permission requires the
copyright and license notice to accompany copies/substantial portions. The
package retains that upstream license in full. XPMP2's own notices apply to
its bundled resources and dependencies.

## X-Plane SDK

The build uses SDK headers made available in the pinned XPMP2 source.
The SDK has its own license, Copyright (c) 2008 Sandy Barbour and Ben Supnik.
Its copyright/conditions/disclaimer must be retained for source redistributions,
and its authors' names, X-Plane and Laminar Research may not be used to imply
endorsement. This repository does not vendor the SDK. Installed packages retain
its license as `LICENSE.XPlaneSDK`.

See the [official SDK](https://developer.x-plane.com/sdk/) for current terms.
X-Plane itself is not part of this software.

## Toolchain and runtime

Nix/nixpkgs, Python, Wine, GCC/MinGW and compiler runtimes are separate build/runtime
dependencies with their own licenses. They are obtained through the locked
nixpkgs input rather than relicensed here. The native plugin statically links
compiler runtimes; the applicable GCC Runtime Library Exception accompanies the
toolchain's terms. Anyone redistributing compiled artifacts must satisfy their
dependency/source/notice obligations; an MIT repository license does not remove
those obligations.

This project initially distributes source, not a bundle of proprietary apps or
preinstalled Wine prefixes.

## BeyondATC and models

BeyondATC, its official X-Plane plugin and CSL model libraries are obtained
separately. Their licenses and purchase/access conditions remain applicable.
Nothing here grants redistribution rights or vendor support. The project does
not ship paid-app patch tools, modified vendor assemblies, vendor-derived
source/disassembly, credentials, recordings or model assets.
