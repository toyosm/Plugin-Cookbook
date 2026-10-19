# Notes for AI agents working in this repository

## If the user wants a new plugin

Follow `prompts/plugin-from-idea.md`: propose the shape, get confirmation, then write
`plugins/<id>.plugin.toml`. Because you can run commands here, also close the loop yourself:

```sh
python3 tools/recipe.py check plugins/<id>.plugin.toml
python3 tools/recipe.py test plugins/<id>.plugin.toml        # fix every FAIL, consider every WARN
python3 tools/recipe.py build <id>                           # optional: full plugin build
```

Compiler errors point at lines inside the `.plugin.toml` (via `#line`).

## If you change the framework

- `framework/include/cookbook/Engine.hpp` is shared by the DPF wrapper and the test harness. Keep it
  real-time safe and keep both paths using it.
- `tools/recipe.py` owns the recipe schema. Any new key must be documented in
  `prompts/plugin-from-idea.md` (the AI-facing spec) and validated (unknown keys are errors).
- After changes run `python3 tools/recipe.py test --all` and `python3 tools/recipe.py build`.
  Optional external validators: `pluginval --strictness-level 10 --validate build/bin/<id>.vst3`,
  `clap-validator validate build/bin/<id>.clap`.
- Keep recipes self-contained: no new include paths or link dependencies for kernel code.
