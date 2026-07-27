# Cookbook

Recipes for the Nomad's Cookbook page. Drop files in here, then turn the page on
in **Admin → Menu Pages → Cookbook** (it ships off).

Subfolders become categories — `Cookbook/Bread/Cornbread.md` shows up under
"Bread". One level deep; anything deeper isn't scanned.

## Writing recipes on the Nomad itself

You don't have to prepare files on a computer. The Cookbook page has a
**＋ New recipe** button that writes straight to this folder, and an **Edit**
button on every recipe. What it saves is ordinary Markdown in the format below,
so a recipe typed on a phone is still a plain file you can open on a PC later.

Editing and deleting follow the admin's **Allow uploads** switch — turn that off
and the Cookbook becomes read-only, without hiding the recipes.

## Timers

Any time written into a step becomes a tappable timer: "bake 20-25 minutes"
gets a **⏲ 20 min** button beside it (ranges start at the low end, so you come
back and check). There's also a manual timer at the bottom of the dock.

Timers keep running while you move around the page, survive a reload, hold the
screen awake, and beep when they finish. They're per-browser, not stored on the
card, so two people cooking from the same Nomad don't fight over them.

## Three formats

Any of them works, and they can be mixed in the same folder.

### Markdown (`.md`) — easiest to type by hand

```markdown
---
title: Skillet Cornbread
servings: 8
time: 35 min
category: Bread
tags: cast iron, quick
source: Grandma
---

A crisp-edged cornbread that comes out of a hot skillet.

## Ingredients
- 1 cup cornmeal
- 1 cup flour
- 1 1/2 tsp salt
- 2 eggs

## Steps
1. Heat the oven to 425°F with the skillet inside.
2. Whisk the dry ingredients together.
3. Bake 20-25 minutes until the top springs back.

## Notes
Buttermilk instead of milk makes it tangier.
```

The `---` block at the top is optional; so is every field in it. A `# Heading`
before the first section works as the title if there's no front matter.

### JSON (`.json`) — easiest to generate

```json
{
  "title": "Skillet Cornbread",
  "servings": 8,
  "time": "35 min",
  "category": "Bread",
  "tags": ["cast iron", "quick"],
  "ingredients": ["1 cup cornmeal", "1 1/2 tsp salt", "2 eggs"],
  "steps": ["Heat the oven to 425°F.", "Bake 20-25 minutes."],
  "notes": "Buttermilk instead of milk makes it tangier."
}
```

Ingredients may also be objects — `{"qty": 1, "unit": "cup", "item": "cornmeal"}` —
which come out as the same scalable line.

### Cooklang (`.cook`) — recipe-as-a-sentence

[Cooklang](https://cooklang.org) tags ingredients, cookware and timers inline in
the method, and the ingredients list builds itself:

```cooklang
>> title: Cooklang Pancakes
>> servings: 4
>> time: 20 minutes
>> course: Breakfast

Whisk the @flour{200%g}, @milk{300%ml} and @eggs{2} in a #mixing bowl{}.

Cook a ladle of batter in a #frying pan{} for ~{2%minutes} until it bubbles,
then flip. Serve with @maple syrup{}.
```

`@ingredient{qty%unit}` (use `{}` when the name is more than one word),
`#cookware{}` and `~timer{5%min}` are lifted out automatically; `>> key: value`
lines are metadata (`servings` still drives scaling). See
`Example - Cooklang Pancakes.cook`.

## Photos

An image with the same name as the recipe becomes its photo:
`Skillet Cornbread.md` + `Skillet Cornbread.jpg`. Or set `image:` in the recipe
to any path on the card.

## Serving scaling

The page reads the leading amount off each ingredient line and multiplies it
when you change the serving count. It understands `2`, `1.5`, `1/2`, `1 1/2`,
`½`, `1½`, and ranges like `2-3` or `2 to 3`.

Lines that don't start with a number — "salt to taste", "a handful of parsley" —
are left exactly as written, because they don't scale. Unit names aren't
converted either: 8 tsp stays 8 tsp rather than becoming 2 tbsp 2 tsp.
