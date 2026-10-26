{
  lib,
  stdenv,
  fetchFromGitHub,
  cmake,
  pkgsCross,
  makeWrapper,
  makeDesktopItem,
  python3,
  wineWow64Packages,
  xvfb-run,
  nix,
  systemd,
}:
let
  version = "0.1.0";
  xpmp2 = fetchFromGitHub {
    owner = "TwinFan";
    repo = "XPMP2";
    rev = "fb543f9d063e7fc61e08f9d21c6e68136606fc23";
    hash = "sha256-smPHCSBunfc3vgshK6EVXYV6NE/tceLXAIc/N18vOWo=";
  };
  companion = pkgsCross.mingwW64.stdenv.mkDerivation {
    pname = "beyondatc-native-companion";
    inherit version;
    src = ../companion;
    buildInputs = [ pkgsCross.mingwW64.windows.mcfgthreads ];
    buildPhase = ''
      runHook preBuild
      ${pkgsCross.mingwW64.stdenv.cc.targetPrefix}windres discovery.rc -o discovery-res.o
      $CC -O2 -Wall -Wextra -Werror -mwindows -static -Wl,--no-insert-timestamp \
        discovery.c discovery-res.o -o X-Plane.exe -lws2_32
      $CC -O2 -Wall -Wextra -Werror -mwindows -static -Wl,--no-insert-timestamp \
        -DPTT_KEY=VK_F24 discovery.c -o NativeBridgePttProbe.exe -lws2_32
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      install -Dm755 X-Plane.exe $out/bin/X-Plane.exe
      install -Dm755 NativeBridgePttProbe.exe $out/bin/NativeBridgePttProbe.exe
      runHook postInstall
    '';
    meta.platforms = lib.platforms.windows;
  };
  wine = wineWow64Packages.stable;
  desktop = makeDesktopItem {
    name = "beyondatc-native";
    desktopName = "BeyondATC (native X-Plane)";
    comment = "Unofficial native Linux X-Plane connector for BeyondATC Experimental";
    exec = "beyondatc-native launch";
    icon = "applications-games";
    categories = [
      "Game"
      "Simulation"
    ];
    startupWMClass = "beyondatc.exe";
  };
in
stdenv.mkDerivation {
  pname = "beyondatc-native";
  inherit version;
  src = lib.cleanSource ../.;
  nativeBuildInputs = [
    cmake
    makeWrapper
  ];
  cmakeFlags = [ "-DXPMP2_SOURCE_DIR=${xpmp2}" ];
  doCheck = true;
  nativeCheckInputs = [
    python3
    wine
    xvfb-run
  ];
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    export HOME="$TMPDIR/beyondatc-test-home"
    export WINEPREFIX="$HOME/wine"
    export WINEDEBUG=-all
    export WINEDLLOVERRIDES="mscoree,mshtml,winemenubuilder.exe=d"
    export BATC_TEST_WINE=${wine}/bin/wine
    export BATC_TEST_SHIM=${companion}/bin/NativeBridgePttProbe.exe
    export PYTHONDONTWRITEBYTECODE=1
    export PYTHONPATH="$PWD/../python"
    mkdir -p "$HOME"
    xvfb-run -a sh -c 'wineboot --init && python3 -m unittest discover -s ../tests -p "test_*.py" -v'
    runHook postCheck
  '';
  postInstall = ''
    install -Dm755 ${companion}/bin/X-Plane.exe $out/libexec/beyondatc-native/X-Plane.exe
    for script in beyondatc-native.py install.py supervisor.py; do
      install -Dm644 ../python/"$script" "$out/libexec/beyondatc-native/$script"
    done
    makeWrapper ${python3}/bin/python3 $out/bin/beyondatc-native \
      --add-flags "$out/libexec/beyondatc-native/beyondatc-native.py" \
      --set BATC_NATIVE_PACKAGE "$out" \
      --set BATC_NATIVE_WINE ${wine}/bin/wine \
      --prefix PATH : ${
        lib.makeBinPath [
          nix
          systemd
        ]
      }
    install -Dm644 ../packaging/beyondatc-native.service "$out/share/systemd/user/beyondatc-native.service"
    substituteInPlace "$out/share/systemd/user/beyondatc-native.service" --replace-fail @out@ "$out"
    mkdir -p $out/share/applications
    cp ${desktop}/share/applications/*.desktop $out/share/applications/
    install -Dm644 ../THIRD_PARTY_NOTICES.md "$out/share/beyondatc-native/THIRD_PARTY_NOTICES.md"
  '';
  passthru = {
    xpmp2Source = xpmp2;
    wineRuntime = wine;
  };
  meta = {
    description = "Unofficial native Linux X-Plane connector and Wine discovery/PTT helpers for BeyondATC";
    homepage = "https://github.com/keskvarc/beyondatc-linux";
    mainProgram = "beyondatc-native";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
