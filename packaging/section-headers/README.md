# Nexus section headers

`svg/` is the single source of record; the sibling `.png`s are what the Nexus page embeds.

To re-rasterize after editing an SVG: wrap it in this two-line HTML shell (the old `_html/`
copies were exactly this — deleted as byte-duplicates of `svg/`), open it in a browser, and
capture the 1300×130 region with a transparent background:

```html
<!doctype html><meta charset="utf-8">
<style>html,body{margin:0;padding:0;background:transparent;overflow:hidden}</style>
<!-- paste the svg/*.svg content here -->
```
