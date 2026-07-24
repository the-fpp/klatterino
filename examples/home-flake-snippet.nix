# flake.nix
{
  inputs = {
    chatterinoPlugins.url = "github:the-fpp/klatterino";
  };

  outputs = { self, nixpkgs, home-manager, chatterinoPlugins, ... }:
  {
    homeConfigurations.YOUR_USER = home-manager.lib.homeManagerConfiguration {
      pkgs = import nixpkgs { system = "x86_64-linux"; };
      modules = [
        chatterinoPlugins.homeModules.default
        {
          programs.chatterinoTabEmit = {
            enable = true;
            flatpak = false; # set true for Flatpak Chatterino
            installListener = true;
          };
        }
      ];
    };
  };
}
