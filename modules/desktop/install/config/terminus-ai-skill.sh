# Place Terminus-Est in each assistant's global skills directory on first install.
mkdir -p ~/.agents/skills ~/.claude/skills ~/.codex/skills ~/.pi/agent/skills
for skills_dir in ~/.agents/skills ~/.claude/skills ~/.codex/skills ~/.pi/agent/skills; do
  ln -sfn "$TERMINUS_DESKTOP_PATH/default/terminus-skill" "$skills_dir/terminus-est"
done
