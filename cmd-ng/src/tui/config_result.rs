use super::config_state::{ConfigConfirmation, ConfigRequest, SavedConfigState};
use crate::persistent_config::PersistentConfigResponse;
use crate::persistent_config_render::error_text;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ConfigJobKind {
    Refresh,
    Save,
    Apply,
    Clear,
}

impl ConfigJobKind {
    pub(super) const fn as_str(self) -> &'static str {
        match self {
            Self::Refresh => "refresh",
            Self::Save => "save",
            Self::Apply => "apply",
            Self::Clear => "clear",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ConfigOutcome {
    Refreshed,
    Saved,
    Applied,
    Cleared,
    AwaitingConfirmation,
    Failed,
}

impl ConfigOutcome {
    pub(super) const fn status(self) -> &'static str {
        match self {
            Self::Refreshed => "Saved Config refreshed",
            Self::Saved => "Saved Config saved",
            Self::Applied => "Saved Config applied",
            Self::Cleared => "Saved Config cleared",
            Self::AwaitingConfirmation => "Saved Config confirmation required",
            Self::Failed => "Saved Config request failed",
        }
    }
}

pub(super) struct ConfigJobResult {
    pub(super) request: ConfigRequest,
    pub(super) mutation: Option<Result<PersistentConfigResponse, String>>,
    pub(super) refresh: Result<PersistentConfigResponse, String>,
}

impl ConfigJobResult {
    pub(super) fn refresh(response: Result<PersistentConfigResponse, String>) -> Self {
        Self {
            request: ConfigRequest::Refresh,
            mutation: None,
            refresh: response,
        }
    }

    pub(super) fn mutation(
        request: ConfigRequest,
        mutation: Result<PersistentConfigResponse, String>,
        refresh: Result<PersistentConfigResponse, String>,
    ) -> Self {
        Self {
            request,
            mutation: Some(mutation),
            refresh,
        }
    }

    pub(super) fn transport(request: ConfigRequest, message: String) -> Self {
        if matches!(request, ConfigRequest::Refresh) {
            return Self::refresh(Err(message));
        }
        Self::mutation(request, Err(message.clone()), Err(message))
    }
}

impl ConfigRequest {
    pub(super) const fn kind(&self) -> ConfigJobKind {
        match self {
            Self::Refresh => ConfigJobKind::Refresh,
            Self::Save { .. } => ConfigJobKind::Save,
            Self::Apply { .. } => ConfigJobKind::Apply,
            Self::Clear => ConfigJobKind::Clear,
        }
    }
}

impl SavedConfigState {
    pub(super) fn request_refresh(&self) -> Option<ConfigRequest> {
        (self.is_supported() && self.busy.is_none()).then_some(ConfigRequest::Refresh)
    }

    pub(super) fn start(&mut self, kind: ConfigJobKind) {
        self.busy = Some(kind);
        self.error = None;
    }

    pub(super) fn finish(&mut self, result: ConfigJobResult) -> ConfigOutcome {
        self.busy = None;
        let refresh_error = match result.refresh {
            Ok(response) if response.envelope.ok => self.apply_authoritative(response).err(),
            Ok(response) => Some(error_text(&response.envelope)),
            Err(error) => Some(error),
        };
        let Some(mutation) = result.mutation else {
            if let Some(error) = refresh_error {
                self.error = Some(error);
                return ConfigOutcome::Failed;
            }
            self.error = None;
            return ConfigOutcome::Refreshed;
        };
        match mutation {
            Err(error) => {
                self.error = Some(error);
                ConfigOutcome::Failed
            }
            Ok(response) if !response.envelope.ok => self.finish_failure(result.request, response),
            Ok(_) => {
                if let Some(error) = refresh_error {
                    self.error = Some(format!("authoritative config refresh failed: {error}"));
                    ConfigOutcome::Failed
                } else {
                    self.error = None;
                    match result.request.kind() {
                        ConfigJobKind::Refresh => ConfigOutcome::Refreshed,
                        ConfigJobKind::Save => ConfigOutcome::Saved,
                        ConfigJobKind::Apply => ConfigOutcome::Applied,
                        ConfigJobKind::Clear => ConfigOutcome::Cleared,
                    }
                }
            }
        }
    }

    fn finish_failure(
        &mut self,
        request: ConfigRequest,
        response: PersistentConfigResponse,
    ) -> ConfigOutcome {
        let confirmation_required = response
            .envelope
            .error
            .as_ref()
            .is_some_and(|error| error.code == "confirmation_required");
        if confirmation_required {
            let dangerous = response.envelope.dangerous_items.clone();
            self.confirmation = match request {
                ConfigRequest::Save { items, .. } => {
                    Some(ConfigConfirmation::Save { items, dangerous })
                }
                ConfigRequest::Apply { .. } => Some(ConfigConfirmation::Apply { dangerous }),
                ConfigRequest::Refresh | ConfigRequest::Clear => None,
            };
            if self.confirmation.is_some() {
                self.error = None;
                return ConfigOutcome::AwaitingConfirmation;
            }
        }
        self.error = Some(error_text(&response.envelope));
        ConfigOutcome::Failed
    }
}
