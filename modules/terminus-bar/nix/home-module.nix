{
  config,
  pkgs,
  lib,
  ...
}:
let
  cfg = config.programs.terminusbar;
  jsonFormat = pkgs.formats.json { };
  tomlFormat = pkgs.formats.toml { };

  generateConfig =
    format: name: value:
    if lib.isString value then
      pkgs.writeText name value
    else if builtins.isPath value || lib.isStorePath value then
      value
    else
      format.generate name value;

  generateToml = generateConfig tomlFormat;
  generateJson = generateConfig jsonFormat;
in
{
  # Disable the home-manager module to avoid conflicts
  # The module got turned into a folder on 2026-09-30. Disable both old and new.
  disabledModules = [
    "programs/terminusbar.nix"
    "programs/terminusbar"
  ];

  imports = [
    (lib.mkRenamedOptionModule
      [ "programs" "terminusbar" "validateConfig" ]
      [ "programs" "terminusbar" "checkConfig" ]
    )
  ];

  options.programs.terminusbar = {
    enable = lib.mkEnableOption "terminusbar, a lightweight Wayland shell and bar";

    systemd.enable = lib.mkEnableOption "a systemd user service for terminusbar";

    package = lib.mkPackageOption pkgs "terminusbar" { nullable = true; };

    checkConfig = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Validate the configuration file at build time.";
    };

    settings = lib.mkOption {
      type =
        with lib.types;
        oneOf [
          tomlFormat.type
          str
          path
        ];
      default = { };
      description = ''
        Default settings for terminusbar, Can be written as:
          - A Nix attrset (converted to TOML via nixpkgs' tomlFormat)
          - A raw TOML string
          - A path to a `.toml` file


        Note: these settings can still be overwritten at runtime via the settings menu.
      '';
      example = lib.literalExpression ''
        shell = {
          font = "JetBrainsMono Nerd Font";
          settings_show_advanced = true;
        };

        theme = {
          mode = "dark";
          source = "builtin";
          builtin = "Catppuccin";
        };
      '';
    };

    customPalettes = lib.mkOption {
      type =
        with lib.types;
        attrsOf (oneOf [
          jsonFormat.type
          str
          path
        ]);
      default = { };
      description = ''
        Custom color pallete options.

      '';
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.user.services.terminusbar = lib.mkIf cfg.systemd.enable {
      Unit = {
        Description = "TerminusBar - A lightweight Wayland shell and bar";
        PartOf = [ config.wayland.systemd.target ];
        After = [ config.wayland.systemd.target ];
        X-Restart-Triggers =
          lib.optional (cfg.settings != { }) "${config.xdg.configFile."terminus-est/terminus-bar/config.toml".source}"
          ++ lib.mapAttrsToList (
            name: _: "${config.xdg.configFile."terminus-est/terminus-bar/palettes/${name}.json".source}"
          ) cfg.customPalettes;
      };

      Service = {
        ExecStart = lib.getExe cfg.package;
        Restart = "on-failure";
      };

      Install.WantedBy = [ config.wayland.systemd.target ];
    };

    home.packages = lib.optional (cfg.package != null) cfg.package;

    xdg = {
      configFile = lib.mkMerge [
        (lib.mkIf (cfg.settings != { }) {
          "terminus-est/terminus-bar/config.toml".source =
            let
              rawConfig = generateToml "config.toml" cfg.settings;
            in
            if cfg.checkConfig && cfg.package != null then
              pkgs.runCommand "terminusbar-config.toml" { } ''
                ${lib.getExe cfg.package} config validate ${rawConfig}
                cp ${rawConfig} $out
              ''
            else
              rawConfig;
        })
        (lib.mapAttrs' (
          name: palette:
          lib.nameValuePair "terminus-est/terminus-bar/palettes/${name}.json" {
            source = generateJson "${name}-palette.json" palette;
          }
        ) cfg.customPalettes)
      ];
    };

    assertions = [
      {
        assertion = !cfg.systemd.enable || cfg.package != null;
        message = "programs.terminusbar.package cannot be null when programs.terminusbar.systemd.enable is true";
      }
    ];
  };

  _class = "homeManager";
}
