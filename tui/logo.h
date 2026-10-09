/* botter logo: 11x12 pixel art (the Botter mascot's head), muted toward the sage UI palette.
 * Each char is a key into LOGO_COLORS; '.' is transparent. Rendered with half blocks. */
static const char *const LOGO_ROWS[] = {
    "..ccccccc..",
    ".eeeeeeeee.",
    ".ggggggggg.",
    ".ijjklmjji.",
    ".oppklmppo.",
    ".rstssstsr.",
    ".vwxwwwxwv.",
    ".yzzzzzzzy.",
    ".AzzCCCzzA.",
    ".ABBBBBBBA.",
    "..DEFGHIJ..",
    "..DEFGHIJ..",
};

static const struct { char key; uint32_t rgb; } LOGO_COLORS[] = {
    {'c', 0xC48479}, {'e', 0xB4756C}, {'g', 0xA36861}, {'i', 0xC98A81},
    {'j', 0x5E6C7A}, {'k', 0x8E5A55}, {'l', 0xAA7069}, {'m', 0xCB8E85},
    {'o', 0xAD6F67}, {'p', 0x5B7181}, {'r', 0xCB8E85}, {'s', 0x5F7F8E},
    {'t', 0xB4DDBE}, {'v', 0xB97A71}, {'w', 0x608A97}, {'x', 0x95C6AE},
    {'y', 0xAE6F67}, {'z', 0x7FA7AE}, {'A', 0x9E625B}, {'B', 0x8AB3B5},
    {'C', 0x9A5650}, {'D', 0x8E5A55}, {'E', 0x97605A}, {'F', 0xA0675F},
    {'G', 0xAA6E66}, {'H', 0xB4756C}, {'I', 0xBE7C73}, {'J', 0xC8847A},
};
