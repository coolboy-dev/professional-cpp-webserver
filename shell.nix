{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [ gcc cmake gnumake gtest wrk apacheHttpd curl netcat python3 ];
}
