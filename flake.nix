{
  description = "BusyBox-ng: A modern, minimalist, highly optimized fork of BusyBox";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" "i686-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      nixpkgsFor = forAllSystems (system: import nixpkgs { inherit system; });
    in
    {
      packages = forAllSystems (system:
        let pkgs = nixpkgsFor.${system}; in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "busybox-ng";
            version = "git";
            src = ./.;

            nativeBuildInputs = with pkgs; [
              gnumake
              gcc
              bison
              flex
              ncurses
            ];

            configurePhase = ''
              # Accept default configuration
              yes "" | make oldconfig
            '';

            buildPhase = ''
              make -j$NIX_BUILD_CORES
            '';

            installPhase = ''
              mkdir -p $out/bin
              cp busybox $out/bin/busybox-ng
              ln -s busybox-ng $out/bin/busybox
            '';

            meta = with pkgs.lib; {
              description = "A modern, minimalist fork of BusyBox";
              license = licenses.gpl2Only;
              platforms = platforms.linux;
            };
          };
        });

      devShells = forAllSystems (system:
        let pkgs = nixpkgsFor.${system}; in
        {
          default = pkgs.mkShell {
            buildInputs = with pkgs; [
              gnumake
              gcc
              bison
              flex
              ncurses
            ];
          };
        });
    };
}
