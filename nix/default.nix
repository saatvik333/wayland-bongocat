{
  lib,
  stdenv,
  wayland,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "wayland-bongocat";
  version = "2.1.1";
  src = ../.;

  # Protocol bindings are pre-generated and committed to git, so
  # wayland-scanner and wayland-protocols are only needed for `make protocols`.
  strictDeps = true;
  buildInputs = [wayland];

  makeFlags = ["release"];

  # Unit tests only; they need no Wayland session.
  doCheck = true;
  checkTarget = "test";

  installPhase = ''
    runHook preInstall

    install -Dm755 build/release/bongocat $out/bin/${finalAttrs.meta.mainProgram}
    install -Dm644 man/bongocat.1 $out/share/man/man1/bongocat.1
    install -Dm644 bongocat.conf.example $out/share/bongocat/bongocat.conf.example

    runHook postInstall
  '';

  meta = {
    description = "Wayland overlay that displays an animated bongo cat reacting to keyboard input";
    homepage = "https://github.com/saatvik333/wayland-bongocat";
    license = lib.licenses.mit;
    maintainers = with lib.maintainers; [voxi0];
    mainProgram = "bongocat";
    platforms = lib.platforms.linux;
  };
})
