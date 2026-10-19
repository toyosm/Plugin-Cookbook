# Contributing

New example recipes, helper DSP blocks and prompt improvements are welcome.

## Adding an example recipe

1. Put it in `examples/<id>.plugin.toml` with a comment at the top saying what it demonstrates.
2. Use `brand = "Plugin Cookbook"`, `brand_id = "Ckbk"` and a `unique_id` not used by another example.
3. `python3 tools/recipe.py test examples/<id>.plugin.toml` must pass with no `FAIL` (and ideally no `WARN`).
4. Build it and load it in at least one DAW. If you can, run `pluginval --strictness-level 10` and
   `clap-validator` on the result.
5. Add it to the example list in `prompts/plugin-from-idea.md` if it shows a technique the others don't.

## Adding a helper to `framework/include/cookbook/dsp.hpp`

- Header-only, single channel, no allocation outside an explicit `allocate()`/`setup()` call.
- Document it in the helper table of `prompts/plugin-from-idea.md` — that table is what AIs read.
- Use it from an example so the test suite exercises it.

## Changing the recipe format

The schema lives in `tools/recipe.py` (`load_recipe`). Update the validator, the generator and the
format section of `prompts/plugin-from-idea.md` together, and keep old recipes working when possible.
