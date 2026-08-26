use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

/// Display-column width of `text` (CJK-wide glyphs count as two columns).
pub(super) fn display_width(text: &str) -> usize {
    UnicodeWidthStr::width(text)
}

pub(super) fn sanitize_display(text: &str) -> String {
    text.chars()
        .filter(|character| !character.is_control())
        .collect()
}

/// Hard clip to at most `max` display columns; a wide glyph that would
/// straddle the boundary is dropped whole rather than split.
pub(super) fn clip_display(text: &str, max: usize) -> String {
    let mut used = 0usize;
    let mut clipped = String::new();
    for ch in text.chars().filter(|character| !character.is_control()) {
        let width = UnicodeWidthChar::width(ch).unwrap_or(0);
        if used + width > max {
            break;
        }
        used += width;
        clipped.push(ch);
    }
    clipped
}
