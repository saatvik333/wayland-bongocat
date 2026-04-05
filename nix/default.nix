{
  lib,
  stdenv,
  pkg-config,
  wayland,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "wayland-bongocat";
  version = "2.0.0";
  src = ../.;

  # Build toolchain and dependencies
  # Protocol bindings are pre-generated and committed to git, so
  # wayland-scanner and wayland-protocols are only needed for `make protocols`.
  strictDeps = true;
  nativeBuildInputs = [pkg-config];
  buildInputs = [
    wayland
  ];

  makeFlags = ["release"];
  installPhase = ''
    runHook preInstall

    # Install binaries
    install -Dm755 build/bongocat $out/bin/${finalAttrs.meta.mainProgram}
    install -Dm755 scripts/find_input_devices.sh $out/bin/bongocat-find-devices
    
    # Install man page
    install -Dm644 man/bongocat.1 $out/share/man/man1/bongocat.1
    install -Dm644 bongocat.conf.example $out/share/bongocat/bongocat.conf.example

    runHook postInstall
  '';

  # Package information
  meta = {
    description = "Delightful Wayland overlay that displays an animated bongo cat reacting to your keyboard input!";
    homepage = "https://github.com/saatvik333/wayland-bongocat";
    license = lib.licenses.mit;
    maintainers = with lib.maintainers; [voxi0];
    mainProgram = "bongocat";
    platforms = lib.platforms.linux;
  };
})
