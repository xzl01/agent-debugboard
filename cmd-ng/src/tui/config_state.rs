use super::config_result::ConfigJobKind;
use crate::persistent_config::{
    ConfigItemId, PersistentConfigItem, PersistentConfigResponse, PersistentConfigStatus,
};
use std::collections::HashSet;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum ConfigRequest {
    Refresh,
    Save {
        items: Vec<ConfigItemId>,
        confirm: bool,
    },
    Clear,
}

#[derive(Debug, Clone)]
pub(super) enum ConfigConfirmation {
    Save {
        items: Vec<ConfigItemId>,
        dangerous: Vec<ConfigItemId>,
    },
}

#[derive(Default)]
pub(super) struct SavedConfigState {
    pub(super) summary: Option<PersistentConfigStatus>,
    pub(super) items: Vec<PersistentConfigItem>,
    selected: HashSet<ConfigItemId>,
    pub(super) cursor: usize,
    pub(super) focused: bool,
    pub(super) loaded: bool,
    pub(super) backend_available: bool,
    pub(super) backend_reason: String,
    pub(super) pending: u32,
    pub(super) busy: Option<ConfigJobKind>,
    pub(super) confirmation: Option<ConfigConfirmation>,
    pub(super) error: Option<String>,
}

impl SavedConfigState {
    pub(super) const fn is_supported(&self) -> bool {
        self.summary.is_some()
    }

    pub(super) fn observe_summary(&mut self, summary: Option<PersistentConfigStatus>) -> bool {
        let changed = match (&self.summary, &summary) {
            (Some(previous), Some(next)) => {
                previous.available != next.available
                    || previous.reason != next.reason
                    || previous.saved_count != next.saved_count
                    || previous.pending_count != next.pending_count
            }
            (None, Some(_)) => true,
            (Some(_), None) | (None, None) => false,
        };
        self.summary = summary;
        if self.summary.is_none() {
            self.items.clear();
            self.selected.clear();
            self.cursor = 0;
            self.loaded = false;
            self.focused = false;
            self.confirmation = None;
        }
        changed && self.is_supported()
    }

    pub(super) fn apply_authoritative(
        &mut self,
        response: PersistentConfigResponse,
    ) -> Result<(), String> {
        if !response.envelope.ok {
            return Err("config GET failed".to_string());
        }
        let backend = response
            .envelope
            .backend
            .ok_or_else(|| "config GET missing backend".to_string())?;
        let pending = response
            .envelope
            .pending
            .ok_or_else(|| "config GET missing pending".to_string())?;
        let previous_ids = self
            .items
            .iter()
            .map(|item| item.id.clone())
            .collect::<HashSet<_>>();
        let previous_cursor = self.items.get(self.cursor).map(|item| item.id.clone());
        let mut selected = HashSet::new();
        for item in &response.envelope.items {
            if previous_ids.contains(&item.id) {
                if self.selected.contains(&item.id) {
                    selected.insert(item.id.clone());
                }
            } else if item.selected {
                selected.insert(item.id.clone());
            }
        }
        self.items = response.envelope.items;
        self.selected = selected;
        self.cursor = previous_cursor
            .and_then(|id| self.items.iter().position(|item| item.id == id))
            .unwrap_or_else(|| self.cursor.min(self.items.len().saturating_sub(1)));
        self.backend_available = backend.available;
        self.backend_reason = backend.reason;
        self.pending = pending;
        self.loaded = true;
        Ok(())
    }

    pub(super) fn focus(&mut self) {
        self.focused = self.is_supported() && self.loaded;
    }

    pub(super) fn blur(&mut self) {
        self.focused = false;
    }

    pub(super) fn move_cursor(&mut self, delta: isize) {
        if self.focused && !self.items.is_empty() {
            self.cursor = self
                .cursor
                .saturating_add_signed(delta)
                .min(self.items.len() - 1);
        }
    }

    pub(super) fn toggle_current(&mut self) {
        if !self.focused {
            return;
        }
        if let Some(item) = self.items.get(self.cursor) {
            if !self.selected.remove(&item.id) {
                self.selected.insert(item.id.clone());
            }
        }
    }

    #[cfg(test)]
    pub(super) fn selected_ids(&self) -> Vec<String> {
        self.items
            .iter()
            .filter(|item| self.selected.contains(&item.id))
            .map(|item| item.id.as_str().to_string())
            .collect()
    }

    pub(super) fn request_save(&mut self) -> Option<ConfigRequest> {
        if !self.is_supported() || self.busy.is_some() {
            return None;
        }
        let items = self
            .items
            .iter()
            .filter(|item| self.selected.contains(&item.id))
            .map(|item| item.id.clone())
            .collect::<Vec<_>>();
        if !self.loaded || items.is_empty() {
            self.error = Some("no saved-config items selected".to_string());
            return None;
        }
        let dangerous = self
            .items
            .iter()
            .filter(|item| self.selected.contains(&item.id) && item.requires_confirm == Some(true))
            .map(|item| item.id.clone())
            .collect::<Vec<_>>();
        if dangerous.is_empty() {
            Some(ConfigRequest::Save {
                items,
                confirm: false,
            })
        } else {
            self.confirmation = Some(ConfigConfirmation::Save { items, dangerous });
            None
        }
    }

    pub(super) fn request_clear(&mut self) -> Option<ConfigRequest> {
        (self.is_supported() && self.loaded && self.busy.is_none()).then_some(ConfigRequest::Clear)
    }

    pub(super) fn confirmation(&self) -> Option<&ConfigConfirmation> {
        self.confirmation.as_ref()
    }

    pub(super) fn confirm(&mut self) -> Option<ConfigRequest> {
        if !self.is_supported() {
            self.confirmation = None;
            return None;
        }
        match self.confirmation.take()? {
            ConfigConfirmation::Save { items, .. } => Some(ConfigRequest::Save {
                items,
                confirm: true,
            }),
        }
    }

    pub(super) fn cancel_confirmation(&mut self) {
        self.confirmation = None;
    }

    pub(super) fn dismiss_error(&mut self) {
        self.error = None;
    }

    pub(super) fn is_selected(&self, id: &ConfigItemId) -> bool {
        self.selected.contains(id)
    }
}
