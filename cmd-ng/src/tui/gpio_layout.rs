#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(super) struct TuiGpioLayout {
    pub(super) pin: u32,
    pub(super) group: Option<String>,
    pub(super) label: Option<String>,
    pub(super) row: Option<u32>,
    pub(super) column: Option<u32>,
}
