agent_manifest writes manifest.md between two marker comments:

  <!-- botter:generated:begin ... -->
  ...
  <!-- botter:generated:end -->

Everything inside is regenerated: files, skills (description from the SKILL.md frontmatter), tools (kind native/script, description from tools/doc/<n>.json). Text outside the markers is kept, so notes and relations written by hand survive. If manifest.md does not exist it is created with a title and a Notes section.
