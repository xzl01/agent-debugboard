use crate::discovery::{Candidate, DiscoveryBackend, LocalInterface};
use crate::json_contract::{JsonError, JSON_SCHEMA};
use anyhow::Result;
use serde_json::json;
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Duration;

#[derive(Default)]
pub(crate) struct FakeDiscovery {
    pub interfaces: Vec<LocalInterface>,
    pub responses: BTreeMap<String, Result<String, JsonError>>,
    pub enumerations: AtomicUsize,
    pub probes: Mutex<Vec<(Candidate, Duration)>>,
}

impl DiscoveryBackend for FakeDiscovery {
    fn interfaces(&self) -> Result<Vec<LocalInterface>> {
        self.enumerations.fetch_add(1, Ordering::SeqCst);
        Ok(self.interfaces.clone())
    }

    fn probe(&self, candidate: &Candidate, timeout: Duration) -> Result<String, JsonError> {
        self.probes
            .lock()
            .unwrap()
            .push((candidate.clone(), timeout));
        self.responses[&candidate.base_url].clone()
    }
}

pub(crate) fn interface(name: &str, ip: &str) -> LocalInterface {
    LocalInterface {
        name: name.into(),
        ip: ip.parse().unwrap(),
        netmask: "255.255.255.252".parse().unwrap(),
        prefixlen: 30,
        up: true,
    }
}

pub(crate) fn status() -> String {
    json!({
        "schema": JSON_SCHEMA, "ok": true, "command": "status",
        "project": "radxa-linkr-debugger", "mcu": "test-mcu",
        "usb": {"product": "test-product"}, "image_version": "test-version",
    })
    .to_string()
}

pub(crate) fn boards(count: usize) -> FakeDiscovery {
    let mut backend = FakeDiscovery::default();
    for n in 0..count {
        backend.interfaces.push(interface(
            &format!("test-{n}"),
            &format!("192.0.2.{}", 4 * n + 2),
        ));
        backend
            .responses
            .insert(format!("http://192.0.2.{}", 4 * n + 1), Ok(status()));
    }
    backend
}

pub(crate) struct NoDiscovery;

impl DiscoveryBackend for NoDiscovery {
    fn interfaces(&self) -> Result<Vec<LocalInterface>> {
        panic!("interface enumeration must be skipped")
    }
    fn probe(&self, _: &Candidate, _: Duration) -> Result<String, JsonError> {
        panic!("network probing must be skipped")
    }
}
