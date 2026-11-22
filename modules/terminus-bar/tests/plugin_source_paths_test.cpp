#include "config/config_types.h"
#include "scripting/plugin_source_paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_source_paths_test: {}", message);
    }
    return condition;
  }

  bool expectPath(const std::filesystem::path& actual, const std::filesystem::path& expected, const char* message) {
    if (actual != expected) {
      std::println(
          stderr, "plugin_source_paths_test: {}\n  actual:   {}\n  expected: {}", message, actual.string(),
          expected.string()
      );
      return false;
    }
    return true;
  }

} // namespace

int main() {
  ::setenv("TERMINUSBAR_STATE_HOME", "/tmp/terminusbar-path-test-state", 1);
  ::setenv("TERMINUSBAR_DATA_HOME", "/tmp/terminusbar-path-test-data", 1);

  const PluginSourceConfig gitSource{
      .kind = PluginSourceKind::Git,
      .name = "official",
      .location = "https://example.invalid/plugins.git",
  };
  const PluginSourceConfig pathSource{
      .kind = PluginSourceKind::Path,
      .name = "dev",
      .location = "~/dev/terminusbar-plugins",
  };

  const std::filesystem::path stateRoot = "/tmp/terminusbar-path-test-state/terminusbar";
  bool ok = true;
  ok = expectPath(
           scripting::plugin_paths::localSourceRoot(), "/tmp/terminusbar-path-test-data/terminusbar/plugins",
           "local source root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::sourceStorageRoot(gitSource), stateRoot / "plugins/sources/official",
           "git source storage root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::gitRepoRoot(gitSource), stateRoot / "plugins/sources/official/repo", "git repo root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::gitMaterializedRoot(gitSource), stateRoot / "plugins/materialized/official",
           "git materialized root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::registryRoot(gitSource), stateRoot / "plugins/materialized/official",
           "git registry root"
       )
      && ok;
  ok = expect(
           scripting::plugin_paths::registryRoot(pathSource).string().ends_with("/dev/terminusbar-plugins"),
           "path source registry root expands user path"
       )
      && ok;
  ok = expect(
           scripting::plugin_paths::pathIsInside("/tmp/terminusbar/a/b", "/tmp/terminusbar"),
           "child path should be inside parent"
       )
      && ok;
  ok = expect(
           !scripting::plugin_paths::pathIsInside("/tmp/terminusbar", "/tmp/terminusbar"),
           "parent path must not count as inside itself"
       )
      && ok;
  ok = expect(
           !scripting::plugin_paths::pathIsInside("/tmp/terminusbar-other/a", "/tmp/terminusbar"),
           "sibling prefix must not count as inside parent"
       )
      && ok;

  return ok ? 0 : 1;
}
