// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

use crate::cli::Cli;
use crate::client::BoardTarget;
use crate::client_url::resolve_base_url;
use crate::json_contract::{JsonError, JSON_SCHEMA};
use anyhow::{Context, Result};
use serde::Serialize;
use serde_json::Value;
use std::collections::BTreeSet;
use std::io::Read;
use std::net::Ipv4Addr;
use std::time::Duration;

const MAX_PARALLEL_PROBES: usize = 8;
const MAX_STATUS_BYTES: u64 = 1024 * 1024;

#[derive(Debug, Clone)]
pub struct LocalInterface {
    pub name: String,
    pub ip: Ipv4Addr,
    pub netmask: Ipv4Addr,
    pub prefixlen: u8,
    pub up: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Serialize)]
pub struct Candidate {
    pub base_url: String,
    pub interface: String,
    pub local_ip: Ipv4Addr,
    pub prefixlen: u8,
    pub netmask: Ipv4Addr,
}

#[derive(Debug, Clone, Serialize)]
pub struct Device {
    #[serde(flatten)]
    pub candidate: Candidate,
    pub project: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mcu: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub usb: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub image_version: Option<Value>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ProbeFailure {
    #[serde(flatten)]
    pub candidate: Candidate,
    pub error: JsonError,
}

#[derive(Debug, Default, Serialize)]
pub struct DiscoveryReport {
    pub devices: Vec<Device>,
    pub failures: Vec<ProbeFailure>,
}

// Both OS enumeration and HTTP are injectable; tests never enumerate the host
// or connect to a board. A probe must honor its per-candidate timeout.
pub trait DiscoveryBackend: Sync {
    fn interfaces(&self) -> Result<Vec<LocalInterface>>;
    fn probe(&self, candidate: &Candidate, timeout: Duration) -> Result<String, JsonError>;
}

pub struct SystemDiscovery;

impl DiscoveryBackend for SystemDiscovery {
    fn interfaces(&self) -> Result<Vec<LocalInterface>> {
        Ok(if_addrs::get_if_addrs()
            .context("enumerate local network interfaces")?
            .into_iter()
            .filter_map(|iface| {
                let up = iface.is_oper_up();
                match iface.addr {
                    if_addrs::IfAddr::V4(addr) => Some(LocalInterface {
                        name: iface.name,
                        ip: addr.ip,
                        netmask: addr.netmask,
                        prefixlen: addr.prefixlen,
                        up,
                    }),
                    if_addrs::IfAddr::V6(_) => None,
                }
            })
            .collect())
    }

    fn probe(&self, candidate: &Candidate, timeout: Duration) -> Result<String, JsonError> {
        // Do not let host proxies, redirects or another interface turn a local
        // /30 peer probe into a request to an unrelated HTTP service.
        let http = reqwest::blocking::Client::builder()
            .timeout(timeout)
            .connect_timeout(timeout)
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .local_address(std::net::IpAddr::V4(candidate.local_ip))
            .build()
            .map_err(transport_error)?;
        let response = http
            .get(format!("{}/api/v1/status", candidate.base_url))
            // Unlike the blocking client's per-operation timeout, the request
            // timeout also bounds the total lifetime of a streaming body.
            .timeout(timeout)
            .header("Accept", "application/json")
            .send()
            .map_err(transport_error)?;
        if !response.status().is_success() {
            return Err(validation_error(format!("HTTP {}", response.status())));
        }
        let mut bytes = Vec::new();
        response
            .take(MAX_STATUS_BYTES + 1)
            .read_to_end(&mut bytes)
            .map_err(transport_error)?;
        if bytes.len() as u64 > MAX_STATUS_BYTES {
            return Err(validation_error("status response exceeds size limit"));
        }
        String::from_utf8(bytes).map_err(validation_error)
    }
}

pub fn configured_url<'a>(cli: &'a Cli, env_url: Option<&'a str>) -> Option<&'a str> {
    cli.url
        .as_deref()
        .or(cli.addr.as_deref())
        .or(cli.port.as_deref())
        .or(env_url)
}

#[derive(Debug)]
pub struct ResolutionError {
    pub error: JsonError,
    pub report: DiscoveryReport,
}

pub fn resolve_target(
    cli: &Cli,
    env_url: Option<&str>,
    backend: &impl DiscoveryBackend,
) -> Result<BoardTarget, ResolutionError> {
    if let Some(input) = configured_url(cli, env_url) {
        let url = resolve_base_url(input);
        let supported_scheme = input
            .trim()
            .split_once("://")
            .is_none_or(|(scheme, _)| matches!(scheme, "http" | "https"));
        let valid = supported_scheme
            && reqwest::Url::parse(&url).is_ok_and(|parsed| {
                matches!(parsed.scheme(), "http" | "https") && parsed.host_str().is_some()
            });
        return if valid {
            Ok(BoardTarget::from(url))
        } else {
            Err(ResolutionError {
                error: JsonError {
                    code: "invalid_url".into(),
                    message: "device URL must be a nonempty HTTP(S) URL".into(),
                },
                report: DiscoveryReport::default(),
            })
        };
    }

    let report = discover(backend, cli.timeout).map_err(|err| ResolutionError {
        error: JsonError {
            code: "discovery_failed".into(),
            message: format!("{err:#}"),
        },
        report: DiscoveryReport::default(),
    })?;
    match report.devices.as_slice() {
        [device] => Ok(BoardTarget::direct(
            device.candidate.base_url.clone(),
            device.candidate.local_ip,
        )),
        devices => Err(ResolutionError {
            error: JsonError {
                code: if devices.is_empty() {
                    "device_not_found"
                } else {
                    "multiple_devices"
                }
                .into(),
                message: if devices.is_empty() {
                    "no verified Linkr device found on active IPv4 /30 interfaces; use discover or --url"
                } else {
                    "multiple Linkr devices found; select one explicitly with --url"
                }
                .into(),
            },
            report,
        }),
    }
}

fn candidates(interfaces: Vec<LocalInterface>) -> Vec<Candidate> {
    let local_ips: BTreeSet<_> = interfaces.iter().map(|iface| iface.ip).collect();
    interfaces
        .into_iter()
        .filter(|iface| {
            iface.up
                // Windows can report a /32 netmask alongside the correct
                // OnLinkPrefixLength; use the authoritative prefix length.
                && iface.prefixlen == 30
                && !iface.ip.is_loopback()
                && !iface.ip.is_unspecified()
                && !iface.ip.is_multicast()
                && matches!(u32::from(iface.ip) & 3, 1 | 2)
        })
        .filter_map(|iface| {
            // A /30 has precisely two usable hosts; neither host position is
            // assumed to belong to the board.
            let peer = Ipv4Addr::from(u32::from(iface.ip) ^ 3);
            (!local_ips.contains(&peer)).then(|| Candidate {
                base_url: format!("http://{peer}"),
                interface: iface.name,
                local_ip: iface.ip,
                prefixlen: iface.prefixlen,
                netmask: iface.netmask,
            })
        })
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect()
}

pub fn discover(backend: &impl DiscoveryBackend, timeout: Duration) -> Result<DiscoveryReport> {
    let candidates = candidates(backend.interfaces()?);
    let mut report = DiscoveryReport::default();
    // Bounded concurrency and a timeout on every request, including body reads.
    // Join in candidate order only for deterministic display, never selection.
    for batch in candidates.chunks(MAX_PARALLEL_PROBES) {
        std::thread::scope(|scope| {
            let probes: Vec<_> = batch
                .iter()
                .map(|candidate| {
                    scope.spawn(move || {
                        backend
                            .probe(candidate, timeout)
                            .and_then(|body| validate_status(candidate.clone(), &body))
                    })
                })
                .collect();
            for (candidate, probe) in batch.iter().zip(probes) {
                match probe
                    .join()
                    .unwrap_or_else(|_| Err(transport_error("probe panicked")))
                {
                    Ok(device) => report.devices.push(device),
                    Err(error) => report.failures.push(ProbeFailure {
                        candidate: candidate.clone(),
                        error,
                    }),
                }
            }
        });
    }
    Ok(report)
}

fn validate_status(candidate: Candidate, body: &str) -> Result<Device, JsonError> {
    let status: Value = serde_json::from_str(body).map_err(validation_error)?;
    for (field, expected) in [
        ("schema", Value::from(JSON_SCHEMA)),
        ("ok", Value::Bool(true)),
        ("command", Value::from("status")),
        ("project", Value::from("radxa-linkr-debugger")),
    ] {
        if status.get(field) != Some(&expected) {
            return Err(validation_error(format!(
                "status requires {field}={expected}"
            )));
        }
    }
    let metadata = |field| status.get(field).filter(|value| !value.is_null()).cloned();
    Ok(Device {
        candidate,
        project: "radxa-linkr-debugger".into(),
        mcu: metadata("mcu"),
        usb: metadata("usb"),
        image_version: metadata("image_version"),
    })
}

fn transport_error(message: impl std::fmt::Display) -> JsonError {
    JsonError {
        code: "transport_error".into(),
        message: message.to_string(),
    }
}

fn validation_error(message: impl std::fmt::Display) -> JsonError {
    JsonError {
        code: "validation_failed".into(),
        message: message.to_string(),
    }
}

#[cfg(test)]
#[path = "discovery_tests.rs"]
mod tests;
