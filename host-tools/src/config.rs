use std::{
    collections::HashSet,
    net::{IpAddr, Ipv4Addr, SocketAddr},
    path::{Path, PathBuf},
    time::Duration,
};

use anyhow::{bail, Context, Result};
use clap::Args;
use url::{Host, Url};

pub const DEFAULT_HOST: IpAddr = IpAddr::V4(Ipv4Addr::LOCALHOST);
pub const DEFAULT_PORT: u16 = 8787;
pub const DEFAULT_BOARD_URL: &str = "http://172.29.203.1:8080";

#[derive(Debug, Clone, Args)]
pub struct ServeOptions {
    /// Loopback address used by the Web UI, device gateway and Serial Broker.
    #[arg(long, env = "LINKR_HOST", default_value = "127.0.0.1")]
    pub host: IpAddr,

    /// Loopback port used by the combined host service.
    #[arg(long, env = "LINKR_BRIDGE_PORT", default_value_t = DEFAULT_PORT)]
    pub port: u16,

    /// Debugger firmware HTTP endpoint on USB-NCM.
    #[arg(long, env = "LINKR_BOARD_URL", default_value = DEFAULT_BOARD_URL)]
    pub board_url: Url,

    /// Built Web UI directory. Defaults to LINKR_WEB_ROOT or a nearby web/dist.
    #[arg(long, env = "LINKR_WEB_ROOT")]
    pub web_root: Option<PathBuf>,

    /// Browser origins allowed to call the loopback gateway.
    #[arg(
        long,
        env = "LINKR_TRUSTED_ORIGINS",
        value_delimiter = ',',
        default_value = "https://xzl01.github.io"
    )]
    pub trusted_origins: Vec<String>,

    /// Keep an unexpectedly disconnected UART open for this many milliseconds.
    #[arg(long, env = "LINKR_SERIAL_IDLE_MS", default_value_t = 30_000)]
    pub serial_idle_ms: u64,
}

impl Default for ServeOptions {
    fn default() -> Self {
        Self {
            host: DEFAULT_HOST,
            port: DEFAULT_PORT,
            board_url: Url::parse(DEFAULT_BOARD_URL).expect("default board URL is valid"),
            web_root: None,
            trusted_origins: vec!["https://xzl01.github.io".to_owned()],
            serial_idle_ms: 30_000,
        }
    }
}

#[derive(Debug, Clone)]
pub struct HostConfig {
    pub bind: SocketAddr,
    pub board_url: Url,
    pub web_root: PathBuf,
    pub trusted_origins: HashSet<String>,
    pub serial_idle_timeout: Duration,
}

impl HostConfig {
    pub fn from_options(options: ServeOptions) -> Result<Self> {
        if !options.host.is_loopback() {
            bail!(
                "refusing to expose an unauthenticated hardware gateway on non-loopback address {}",
                options.host
            );
        }
        if options.board_url.scheme() != "http" && options.board_url.scheme() != "https" {
            bail!("board URL must use http or https");
        }
        let web_root = match options.web_root {
            Some(path) => {
                if !has_web_index(&path) {
                    bail!(
                        "Web UI directory {} does not contain index.html",
                        path.display()
                    );
                }
                path.canonicalize()
                    .with_context(|| format!("resolve Web UI directory {}", path.display()))?
            }
            None => discover_web_root()?,
        };
        Ok(Self {
            bind: SocketAddr::new(options.host, options.port),
            board_url: options.board_url,
            web_root,
            trusted_origins: options.trusted_origins.into_iter().collect(),
            serial_idle_timeout: Duration::from_millis(options.serial_idle_ms.max(1_000)),
        })
    }

    pub fn public_url(&self) -> String {
        format!("http://{}:{}/", self.bind.ip(), self.bind.port())
    }
}

fn has_web_index(path: &Path) -> bool {
    path.join("index.html").is_file()
}

pub fn discover_web_root() -> Result<PathBuf> {
    let mut candidates = Vec::new();
    if let Ok(root) = std::env::var("LINKR_WEB_ROOT") {
        candidates.push(PathBuf::from(root));
    }
    if let Ok(cwd) = std::env::current_dir() {
        candidates.push(cwd.join("web/dist"));
        candidates.push(cwd.join("../web/dist"));
        candidates.push(cwd.join("share/radxa-linkr-debugger/web"));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            candidates.push(parent.join("web"));
            candidates.push(parent.join("../share/radxa-linkr-debugger/web"));
            candidates.push(parent.join("../../../web/dist"));
        }
    }
    if let Some(path) = candidates.into_iter().find(|path| has_web_index(path)) {
        return path
            .canonicalize()
            .with_context(|| format!("resolve Web UI directory {}", path.display()));
    }
    bail!("cannot find a built Web UI; run `npm --prefix web run build` or pass --web-root")
}

pub fn is_allowed_origin(origin: Option<&str>, trusted: &HashSet<String>) -> bool {
    let Some(origin) = origin else {
        return true;
    };
    if trusted.contains(origin) {
        return true;
    }
    Url::parse(origin).is_ok_and(|url| {
        url.scheme() == "http"
            && url.host().is_some_and(|host| match host {
                Host::Domain(name) => name.eq_ignore_ascii_case("localhost"),
                Host::Ipv4(address) => address.is_loopback(),
                Host::Ipv6(address) => address.is_loopback(),
            })
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_loopback_http_origins_are_implicitly_trusted() {
        let trusted = HashSet::from(["https://xzl01.github.io".to_owned()]);
        assert!(is_allowed_origin(None, &trusted));
        assert!(is_allowed_origin(Some("http://127.0.0.1:5173"), &trusted));
        assert!(is_allowed_origin(Some("http://127.42.0.1:5173"), &trusted));
        assert!(is_allowed_origin(Some("http://localhost:8787"), &trusted));
        assert!(is_allowed_origin(Some("http://[::1]:8787"), &trusted));
        assert!(is_allowed_origin(Some("https://xzl01.github.io"), &trusted));
        assert!(!is_allowed_origin(Some("https://evil.example"), &trusted));
        assert!(!is_allowed_origin(Some("not a URL"), &trusted));
    }

    #[test]
    fn refuses_non_loopback_bind_addresses() {
        let directory = tempfile::tempdir().unwrap();
        std::fs::write(directory.path().join("index.html"), "<!doctype html>").unwrap();
        let options = ServeOptions {
            host: "0.0.0.0".parse().unwrap(),
            web_root: Some(directory.path().to_owned()),
            ..ServeOptions::default()
        };
        assert!(HostConfig::from_options(options).is_err());
    }

    #[test]
    fn explicit_web_root_requires_index() {
        let directory = tempfile::tempdir().unwrap();
        let options = ServeOptions {
            web_root: Some(directory.path().to_owned()),
            ..ServeOptions::default()
        };
        assert!(HostConfig::from_options(options).is_err());
    }
}
