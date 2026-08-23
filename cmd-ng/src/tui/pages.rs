use super::model::TuiModel;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub(super) enum ActivePage {
    #[default]
    Controls,
    SavedConfig,
    Status,
}

impl ActivePage {
    pub(super) const fn next(self) -> Self {
        match self {
            Self::Controls => Self::SavedConfig,
            Self::SavedConfig => Self::Status,
            Self::Status => Self::Controls,
        }
    }

    pub(super) const fn prev(self) -> Self {
        match self {
            Self::Controls => Self::Status,
            Self::SavedConfig => Self::Controls,
            Self::Status => Self::SavedConfig,
        }
    }
}

pub(super) const fn clamp_scroll(scroll: usize, content_height: usize, viewport: usize) -> usize {
    let max = content_height.saturating_sub(viewport);
    if scroll > max {
        max
    } else {
        scroll
    }
}

pub(super) const fn ensure_visible(
    selection: usize,
    scroll: usize,
    viewport: usize,
    content_height: usize,
) -> usize {
    let mut next = scroll;
    if selection < next {
        next = selection;
    } else if viewport > 0 && selection >= next + viewport {
        next = selection + 1 - viewport;
    }
    clamp_scroll(next, content_height, viewport)
}

impl TuiModel {
    pub(super) fn set_page(&mut self, page: ActivePage) {
        if page == self.active_page {
            return;
        }
        if self.active_page == ActivePage::SavedConfig {
            self.saved_config.blur();
        }
        self.active_page = page;
        if page == ActivePage::SavedConfig {
            self.saved_config.focus();
        }
    }

    pub(super) fn next_page(&mut self) {
        self.set_page(self.active_page.next());
    }

    pub(super) fn prev_page(&mut self) {
        self.set_page(self.active_page.prev());
    }

    pub(super) fn page_scroll(&self) -> usize {
        match self.active_page {
            ActivePage::Controls => self.controls_scroll,
            ActivePage::SavedConfig => self.config_scroll,
            ActivePage::Status => self.status_scroll,
        }
    }

    pub(super) fn set_page_scroll(&mut self, offset: usize) {
        match self.active_page {
            ActivePage::Controls => self.controls_scroll = offset,
            ActivePage::SavedConfig => self.config_scroll = offset,
            ActivePage::Status => self.status_scroll = offset,
        }
    }
}
