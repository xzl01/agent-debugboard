use super::pages::ActivePage;
use crate::persistent_config::ConfigItemId;
use std::marker::PhantomData;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct TabTarget(pub(super) ActivePage);

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct SavedConfigRowTarget(pub(super) ConfigItemId);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ModalAction {
    Confirm,
    Cancel,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct HardwareModalScope;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct SavedConfigModalScope;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct ModalTarget<Scope> {
    action: ModalAction,
    scope: PhantomData<Scope>,
}

impl<Scope> ModalTarget<Scope> {
    pub(super) const fn confirm() -> Self {
        Self {
            action: ModalAction::Confirm,
            scope: PhantomData,
        }
    }

    pub(super) const fn cancel() -> Self {
        Self {
            action: ModalAction::Cancel,
            scope: PhantomData,
        }
    }

    pub(super) const fn action(self) -> ModalAction {
        self.action
    }
}

pub(super) type HardwareModalTarget = ModalTarget<HardwareModalScope>;
pub(super) type SavedConfigModalTarget = ModalTarget<SavedConfigModalScope>;
