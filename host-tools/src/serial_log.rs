#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::{
    collections::HashMap,
    fs::{self, File, OpenOptions},
    io::{BufWriter, Write},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant, SystemTime},
};

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use time::{format_description::well_known::Iso8601, OffsetDateTime};
use tokio::sync::mpsc;
use tracing::warn;
use uuid::Uuid;

pub const SCHEMA: &str = "linkr-serial-log.v1";
const DEFAULT_QUEUE_RECORDS: usize = 512;

fn reject_symlink(path: &Path) -> std::io::Result<()> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() => Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            format!("refusing symlinked archive path {}", path.display()),
        )),
        Ok(_) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error),
    }
}

fn create_private_dir_all(path: &Path) -> std::io::Result<()> {
    reject_symlink(path)?;
    fs::create_dir_all(path)?;
    reject_symlink(path)?;
    #[cfg(unix)]
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
    Ok(())
}

#[cfg(unix)]
fn harden_archive_permissions(root: &Path) -> std::io::Result<()> {
    reject_symlink(root)?;
    fs::set_permissions(root, fs::Permissions::from_mode(0o700))?;
    for date in fs::read_dir(root)? {
        let date = date?;
        if !date.file_type()?.is_dir() {
            continue;
        }
        fs::set_permissions(date.path(), fs::Permissions::from_mode(0o700))?;
        for session in fs::read_dir(date.path())? {
            let session = session?;
            if !session.file_type()?.is_dir() {
                continue;
            }
            fs::set_permissions(session.path(), fs::Permissions::from_mode(0o700))?;
            for artifact in fs::read_dir(session.path())? {
                let artifact = artifact?;
                if artifact.file_type()?.is_file() {
                    fs::set_permissions(artifact.path(), fs::Permissions::from_mode(0o600))?;
                }
            }
        }
    }
    Ok(())
}

#[cfg(not(unix))]
fn harden_archive_permissions(_root: &Path) -> std::io::Result<()> {
    Ok(())
}

fn private_file_options() -> OpenOptions {
    let mut options = OpenOptions::new();
    options.create(true).write(true);
    #[cfg(unix)]
    options.mode(0o600);
    options
}

fn enforce_private_file(file: &File) -> std::io::Result<()> {
    #[cfg(unix)]
    file.set_permissions(fs::Permissions::from_mode(0o600))?;
    Ok(())
}

fn open_private_append(path: &Path) -> std::io::Result<File> {
    reject_symlink(path)?;
    let file = private_file_options().append(true).open(path)?;
    enforce_private_file(&file)?;
    Ok(file)
}

fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    reject_symlink(path)?;
    let mut file = private_file_options().truncate(true).open(path)?;
    enforce_private_file(&file)?;
    file.write_all(bytes)
}

fn create_private_file(path: &Path) -> std::io::Result<File> {
    reject_symlink(path)?;
    let file = private_file_options().truncate(true).open(path)?;
    enforce_private_file(&file)?;
    Ok(file)
}

#[derive(Debug, Clone)]
pub struct SerialLogConfig {
    pub enabled: bool,
    pub root: PathBuf,
    pub segment_bytes: u64,
    pub total_bytes: u64,
    pub retention: Duration,
    pub queue_records: usize,
}

#[derive(Debug, Clone, Serialize)]
pub struct SerialLogStatus {
    pub schema: &'static str,
    pub enabled: bool,
    pub root: String,
    pub state: &'static str,
    pub active_sessions: usize,
    pub queued_records: usize,
    pub total_bytes: u64,
    pub quota_bytes: u64,
    pub dropped_bytes: u64,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SerialLogManifest {
    pub schema: String,
    pub session_id: String,
    pub channel: String,
    pub device_path: String,
    pub baud: u32,
    pub started_at: String,
    pub ended_at: Option<String>,
    pub status: String,
    pub complete: bool,
    pub pinned: bool,
    pub bytes: u64,
    pub records: u64,
    pub segments: u32,
    pub dropped_bytes: u64,
    pub end_reason: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct SerialLogSummary {
    #[serde(flatten)]
    pub manifest: SerialLogManifest,
    pub directory: String,
}

#[derive(Debug)]
enum Command {
    Start {
        session_id: Uuid,
        channel: String,
        device_path: String,
        baud: u32,
        started_at: OffsetDateTime,
    },
    Data {
        session_id: Uuid,
        bytes: Vec<u8>,
        wall_time: OffsetDateTime,
        host_t_mono_us: u64,
    },
    Finish {
        session_id: Uuid,
        reason: String,
        ended_at: OffsetDateTime,
    },
    Shutdown(tokio::sync::oneshot::Sender<()>),
}

#[derive(Debug, Default)]
struct SessionLoss {
    dropped_bytes: u64,
    failed: bool,
}

#[derive(Debug, Clone, Copy, Default)]
struct ActiveProgress {
    bytes: u64,
    records: u64,
    segments: u32,
}

#[derive(Debug, Default)]
struct RuntimeStatus {
    active_sessions: usize,
    queued_records: usize,
    dropped_bytes: u64,
    session_loss: HashMap<Uuid, SessionLoss>,
    active_progress: HashMap<Uuid, ActiveProgress>,
    last_error: Option<String>,
}

#[derive(Clone)]
pub struct SerialLogService {
    config: Arc<SerialLogConfig>,
    sender: Option<mpsc::Sender<Command>>,
    status: Arc<Mutex<RuntimeStatus>>,
    archive_bytes: Arc<AtomicU64>,
    storage_lock: Arc<Mutex<()>>,
    started: Instant,
}

impl SerialLogService {
    pub async fn new(config: SerialLogConfig) -> Result<Self> {
        let status = Arc::new(Mutex::new(RuntimeStatus::default()));
        let archive_bytes = Arc::new(AtomicU64::new(0));
        let storage_lock = Arc::new(Mutex::new(()));
        if !config.enabled {
            return Ok(Self {
                config: Arc::new(config),
                sender: None,
                status,
                archive_bytes,
                storage_lock,
                started: Instant::now(),
            });
        }

        if let Err(error) = create_private_dir_all(&config.root)
            .with_context(|| format!("create serial log directory {}", config.root.display()))
            .and_then(|()| {
                harden_archive_permissions(&config.root)
                    .context("harden existing serial log permissions")
            })
            .and_then(|()| recover_interrupted(&config.root))
        {
            status.lock().expect("serial log status").last_error = Some(error.to_string());
            warn!(error = %error, "serial log storage unavailable; UART forwarding will continue");
            return Ok(Self {
                config: Arc::new(config),
                sender: None,
                status,
                archive_bytes,
                storage_lock,
                started: Instant::now(),
            });
        }
        archive_bytes.store(
            unpinned_archive_size(&config.root).unwrap_or_default(),
            Ordering::Relaxed,
        );
        let (sender, receiver) = mpsc::channel(config.queue_records.max(1));
        let service = Self {
            config: Arc::new(config.clone()),
            sender: Some(sender),
            status: status.clone(),
            archive_bytes: archive_bytes.clone(),
            storage_lock: storage_lock.clone(),
            started: Instant::now(),
        };
        tokio::task::spawn_blocking(move || {
            writer_loop(config, status, archive_bytes, storage_lock, receiver)
        });
        Ok(service)
    }

    pub fn disabled(root: PathBuf) -> Self {
        Self {
            config: Arc::new(SerialLogConfig {
                enabled: false,
                root,
                segment_bytes: 64 * 1024 * 1024,
                total_bytes: 2 * 1024 * 1024 * 1024,
                retention: Duration::from_secs(30 * 24 * 60 * 60),
                queue_records: DEFAULT_QUEUE_RECORDS,
            }),
            sender: None,
            status: Arc::new(Mutex::new(RuntimeStatus::default())),
            archive_bytes: Arc::new(AtomicU64::new(0)),
            storage_lock: Arc::new(Mutex::new(())),
            started: Instant::now(),
        }
    }

    pub fn start_session(&self, channel: &str, device_path: &str, baud: u32) -> Option<Uuid> {
        let sender = self.sender.as_ref()?;
        let session_id = Uuid::new_v4();
        self.status
            .lock()
            .expect("serial log status")
            .session_loss
            .insert(session_id, SessionLoss::default());
        let command = Command::Start {
            session_id,
            channel: channel.to_owned(),
            device_path: device_path.to_owned(),
            baud,
            started_at: OffsetDateTime::now_utc(),
        };
        if self.try_enqueue(sender, command).is_err() {
            self.status
                .lock()
                .expect("serial log status")
                .session_loss
                .remove(&session_id);
            self.note_drop(
                None,
                0,
                "serial log command queue is full while starting a session",
            );
            return None;
        }
        Some(session_id)
    }

    pub fn record_rx(&self, session_id: Uuid, bytes: Vec<u8>) {
        let Some(sender) = self.sender.as_ref() else {
            return;
        };
        let len = bytes.len() as u64;
        let command = Command::Data {
            session_id,
            bytes,
            wall_time: OffsetDateTime::now_utc(),
            host_t_mono_us: self.started.elapsed().as_micros().min(u64::MAX as u128) as u64,
        };
        if self.try_enqueue(sender, command).is_err() {
            self.note_drop(
                Some(session_id),
                len,
                "serial log queue overflow; UART forwarding continued",
            );
        }
    }

    pub async fn finish_session(&self, session_id: Uuid, reason: impl Into<String>) {
        let Some(sender) = self.sender.as_ref() else {
            return;
        };
        let command = Command::Finish {
            session_id,
            reason: reason.into(),
            ended_at: OffsetDateTime::now_utc(),
        };
        self.status
            .lock()
            .expect("serial log status")
            .queued_records += 1;
        if sender.send(command).await.is_err() {
            let mut status = self.status.lock().expect("serial log status");
            status.queued_records = status.queued_records.saturating_sub(1);
            drop(status);
            self.note_drop(
                Some(session_id),
                0,
                "serial log writer stopped before finishing a session",
            );
        }
    }

    pub async fn shutdown(&self) {
        if let Some(sender) = &self.sender {
            let (done, wait) = tokio::sync::oneshot::channel();
            if sender.send(Command::Shutdown(done)).await.is_ok() {
                let _ = wait.await;
            }
        }
    }

    pub fn status(&self) -> SerialLogStatus {
        let runtime = self.status.lock().expect("serial log status");
        SerialLogStatus {
            schema: SCHEMA,
            enabled: self.config.enabled,
            root: self.config.root.display().to_string(),
            state: if !self.config.enabled {
                "disabled"
            } else if runtime.last_error.is_some() {
                "degraded"
            } else {
                "ready"
            },
            active_sessions: runtime.active_sessions,
            queued_records: runtime.queued_records,
            total_bytes: self.archive_bytes.load(Ordering::Relaxed),
            quota_bytes: self.config.total_bytes,
            dropped_bytes: runtime.dropped_bytes,
            last_error: runtime.last_error.clone(),
        }
    }

    pub fn list(&self) -> Result<Vec<SerialLogSummary>> {
        let mut logs = list_manifests(&self.config.root)?;
        let progress = self
            .status
            .lock()
            .expect("serial log status")
            .active_progress
            .clone();
        for log in &mut logs {
            let Ok(session_id) = Uuid::parse_str(&log.manifest.session_id) else {
                continue;
            };
            if let Some(active) = progress.get(&session_id) {
                log.manifest.bytes = active.bytes;
                log.manifest.records = active.records;
                log.manifest.segments = active.segments;
            }
        }
        Ok(logs)
    }

    pub fn find(&self, session_id: Uuid) -> Result<Option<SerialLogSummary>> {
        Ok(self
            .list()?
            .into_iter()
            .find(|entry| entry.manifest.session_id == session_id.to_string()))
    }

    pub fn set_pinned(&self, session_id: Uuid, pinned: bool) -> Result<SerialLogSummary> {
        let _storage = self.storage_lock.lock().expect("serial log storage");
        let Some(mut summary) = list_manifests(&self.config.root)?
            .into_iter()
            .find(|entry| entry.manifest.session_id == session_id.to_string())
        else {
            bail!("serial log session not found");
        };
        if summary.manifest.status == "recording" {
            bail!("cannot change pin state for an active serial log session");
        }
        let directory = self.config.root.join(&summary.directory);
        let marker = directory.join(".pinned");
        if pinned && !summary.manifest.pinned {
            let size = directory_size(&directory)?;
            create_private_file(&marker).with_context(|| format!("create {}", marker.display()))?;
            subtract_atomic(&self.archive_bytes, size);
        } else if !pinned && summary.manifest.pinned && marker.exists() {
            fs::remove_file(&marker).with_context(|| format!("remove {}", marker.display()))?;
            self.archive_bytes
                .fetch_add(directory_size(&directory)?, Ordering::Relaxed);
        }
        summary.manifest.pinned = pinned;
        Ok(summary)
    }

    pub fn delete(&self, session_id: Uuid) -> Result<()> {
        let _storage = self.storage_lock.lock().expect("serial log storage");
        let Some(summary) = list_manifests(&self.config.root)?
            .into_iter()
            .find(|entry| entry.manifest.session_id == session_id.to_string())
        else {
            bail!("serial log session not found");
        };
        if summary.manifest.status == "recording" {
            bail!("cannot delete an active serial log session");
        }
        if summary.manifest.pinned {
            bail!("unpin the serial log before deleting it");
        }
        let directory = self.config.root.join(summary.directory);
        let size = directory_size(&directory)?;
        fs::remove_dir_all(&directory)
            .with_context(|| format!("delete serial log {}", directory.display()))?;
        subtract_atomic(&self.archive_bytes, size);
        Ok(())
    }

    pub fn read_artifact(&self, session_id: Uuid, format: &str) -> Result<(Vec<u8>, &'static str)> {
        let Some(summary) = self.find(session_id)? else {
            bail!("serial log session not found");
        };
        let directory = self.config.root.join(summary.directory);
        match format {
            "raw" => Ok((
                read_matching(&directory, "rx-", ".bin")?,
                "application/octet-stream",
            )),
            "text" => {
                let raw = read_matching(&directory, "rx-", ".bin")?;
                Ok((
                    String::from_utf8_lossy(&raw).into_owned().into_bytes(),
                    "text/plain; charset=utf-8",
                ))
            }
            "ndjson" => Ok((
                read_matching(&directory, "rx-", ".ndjson")?,
                "application/x-ndjson",
            )),
            _ => bail!("unsupported serial log download format"),
        }
    }

    pub fn artifact_paths(&self, session_id: Uuid, format: &str) -> Result<Vec<PathBuf>> {
        let Some(summary) = self.find(session_id)? else {
            bail!("serial log session not found");
        };
        let directory = self.config.root.join(summary.directory);
        match format {
            "raw" | "text" => matching_paths(&directory, "rx-", ".bin"),
            "ndjson" => matching_paths(&directory, "rx-", ".ndjson"),
            _ => bail!("unsupported streaming serial log format"),
        }
    }

    fn note_drop(&self, session_id: Option<Uuid>, bytes: u64, message: &str) {
        let mut status = self.status.lock().expect("serial log status");
        status.dropped_bytes = status.dropped_bytes.saturating_add(bytes);
        if let Some(session_id) = session_id {
            let loss = status.session_loss.entry(session_id).or_default();
            loss.dropped_bytes = loss.dropped_bytes.saturating_add(bytes);
            loss.failed = true;
        }
        let should_warn = status.last_error.as_deref() != Some(message);
        status.last_error = Some(message.to_owned());
        drop(status);
        if should_warn {
            warn!(dropped_bytes = bytes, "{message}");
        }
    }

    fn try_enqueue(
        &self,
        sender: &mpsc::Sender<Command>,
        command: Command,
    ) -> std::result::Result<(), mpsc::error::TrySendError<Command>> {
        self.status
            .lock()
            .expect("serial log status")
            .queued_records += 1;
        if let Err(error) = sender.try_send(command) {
            let mut status = self.status.lock().expect("serial log status");
            status.queued_records = status.queued_records.saturating_sub(1);
            return Err(error);
        }
        Ok(())
    }
}

struct ActiveSession {
    directory: PathBuf,
    manifest: SerialLogManifest,
    raw: BufWriter<File>,
    index: BufWriter<File>,
    segment: u32,
    segment_bytes: u64,
    sequence: u64,
    writer_failed: bool,
}

fn writer_loop(
    config: SerialLogConfig,
    status: Arc<Mutex<RuntimeStatus>>,
    archive_bytes: Arc<AtomicU64>,
    storage_lock: Arc<Mutex<()>>,
    mut receiver: mpsc::Receiver<Command>,
) {
    let mut sessions = HashMap::<Uuid, ActiveSession>::new();
    {
        let _storage = storage_lock.lock().expect("serial log storage");
        let _ = enforce_retention(&config.root, config.total_bytes, config.retention);
        archive_bytes.store(
            unpinned_archive_size(&config.root).unwrap_or_default(),
            Ordering::Relaxed,
        );
    }
    while let Some(command) = receiver.blocking_recv() {
        {
            let mut runtime = status.lock().expect("serial log status");
            runtime.queued_records = runtime.queued_records.saturating_sub(1);
        }
        let result = match command {
            Command::Start {
                session_id,
                channel,
                device_path,
                baud,
                started_at,
            } => {
                let _storage = storage_lock.lock().expect("serial log storage");
                if let Ok(freed) =
                    enforce_retention(&config.root, config.total_bytes, config.retention)
                {
                    subtract_atomic(&archive_bytes, freed);
                }
                let result = start_writer_session(
                    &config,
                    &mut sessions,
                    session_id,
                    channel,
                    device_path,
                    baud,
                    started_at,
                );
                if result.is_ok() {
                    if let Some(session) = sessions.get(&session_id) {
                        archive_bytes.fetch_add(
                            directory_size(&session.directory).unwrap_or_default(),
                            Ordering::Relaxed,
                        );
                    }
                    status
                        .lock()
                        .expect("serial log status")
                        .active_progress
                        .insert(
                            session_id,
                            ActiveProgress {
                                segments: 1,
                                ..ActiveProgress::default()
                            },
                        );
                } else {
                    status
                        .lock()
                        .expect("serial log status")
                        .session_loss
                        .remove(&session_id);
                }
                result
            }
            Command::Data {
                session_id,
                bytes,
                wall_time,
                host_t_mono_us,
            } => {
                let result = {
                    let _storage = storage_lock.lock().expect("serial log storage");
                    write_data(
                        &config,
                        &mut sessions,
                        &archive_bytes,
                        session_id,
                        &bytes,
                        wall_time,
                        host_t_mono_us,
                    )
                };
                let progress = sessions.get(&session_id).map(|session| ActiveProgress {
                    bytes: session.manifest.bytes,
                    records: session.manifest.records,
                    segments: session.manifest.segments,
                });
                if result.is_err() {
                    let mut runtime = status.lock().expect("serial log status");
                    runtime.dropped_bytes =
                        runtime.dropped_bytes.saturating_add(bytes.len() as u64);
                    let loss = runtime.session_loss.entry(session_id).or_default();
                    loss.dropped_bytes = loss.dropped_bytes.saturating_add(bytes.len() as u64);
                    loss.failed = true;
                } else if let Some(progress) = progress {
                    status
                        .lock()
                        .expect("serial log status")
                        .active_progress
                        .insert(session_id, progress);
                }
                result
            }
            Command::Finish {
                session_id,
                reason,
                ended_at,
            } => {
                let loss = status
                    .lock()
                    .expect("serial log status")
                    .session_loss
                    .remove(&session_id)
                    .unwrap_or_default();
                let result = {
                    let _storage = storage_lock.lock().expect("serial log storage");
                    let manifest_path = sessions
                        .get(&session_id)
                        .map(|session| session.directory.join("manifest.json"));
                    let old_manifest_size = manifest_path
                        .as_ref()
                        .and_then(|path| fs::metadata(path).ok())
                        .map_or(0, |metadata| metadata.len());
                    let result =
                        finish_writer_session(&mut sessions, session_id, reason, ended_at, loss);
                    if result.is_ok() {
                        let new_manifest_size = manifest_path
                            .as_ref()
                            .and_then(|path| fs::metadata(path).ok())
                            .map_or(old_manifest_size, |metadata| metadata.len());
                        adjust_atomic(&archive_bytes, old_manifest_size, new_manifest_size);
                    }
                    result
                };
                status
                    .lock()
                    .expect("serial log status")
                    .active_progress
                    .remove(&session_id);
                result
            }
            Command::Shutdown(done) => {
                let _storage = storage_lock.lock().expect("serial log storage");
                let ids = sessions.keys().copied().collect::<Vec<_>>();
                for id in ids {
                    let manifest_path = sessions
                        .get(&id)
                        .map(|session| session.directory.join("manifest.json"));
                    let old_manifest_size = manifest_path
                        .as_ref()
                        .and_then(|path| fs::metadata(path).ok())
                        .map_or(0, |metadata| metadata.len());
                    let loss = status
                        .lock()
                        .expect("serial log status")
                        .session_loss
                        .remove(&id)
                        .unwrap_or_default();
                    if finish_writer_session(
                        &mut sessions,
                        id,
                        "host shutdown".to_owned(),
                        OffsetDateTime::now_utc(),
                        loss,
                    )
                    .is_ok()
                    {
                        let new_manifest_size = manifest_path
                            .as_ref()
                            .and_then(|path| fs::metadata(path).ok())
                            .map_or(old_manifest_size, |metadata| metadata.len());
                        adjust_atomic(&archive_bytes, old_manifest_size, new_manifest_size);
                    }
                    status
                        .lock()
                        .expect("serial log status")
                        .active_progress
                        .remove(&id);
                }
                let _ = done.send(());
                break;
            }
        };
        if let Err(error) = result {
            status.lock().expect("serial log status").last_error = Some(error.to_string());
            warn!(error = %error, "serial log writer error; UART forwarding continued");
        }
        status.lock().expect("serial log status").active_sessions = sessions.len();
    }
}

#[allow(clippy::too_many_arguments)]
fn start_writer_session(
    config: &SerialLogConfig,
    sessions: &mut HashMap<Uuid, ActiveSession>,
    session_id: Uuid,
    channel: String,
    device_path: String,
    baud: u32,
    started_at: OffsetDateTime,
) -> Result<()> {
    let date = started_at.date().to_string();
    let date_directory = config.root.join(date);
    create_private_dir_all(&date_directory)
        .with_context(|| format!("create serial log date {}", date_directory.display()))?;
    let directory = date_directory.join(format!("{session_id}-{channel}"));
    create_private_dir_all(&directory)
        .with_context(|| format!("create serial log session {}", directory.display()))?;
    let (raw, index) = open_segment(&directory, 1)?;
    let manifest = SerialLogManifest {
        schema: SCHEMA.to_owned(),
        session_id: session_id.to_string(),
        channel,
        device_path,
        baud,
        started_at: format_time(started_at),
        ended_at: None,
        status: "recording".to_owned(),
        complete: false,
        pinned: false,
        bytes: 0,
        records: 0,
        segments: 1,
        dropped_bytes: 0,
        end_reason: None,
    };
    write_manifest(&directory, &manifest)?;
    sessions.insert(
        session_id,
        ActiveSession {
            directory,
            manifest,
            raw,
            index,
            segment: 1,
            segment_bytes: 0,
            sequence: 0,
            writer_failed: false,
        },
    );
    Ok(())
}

fn write_data(
    config: &SerialLogConfig,
    sessions: &mut HashMap<Uuid, ActiveSession>,
    archive_bytes: &AtomicU64,
    session_id: Uuid,
    bytes: &[u8],
    wall_time: OffsetDateTime,
    host_t_mono_us: u64,
) -> Result<()> {
    let Some(session) = sessions.get_mut(&session_id) else {
        return Ok(());
    };
    let result = (|| {
        let rotate = session.segment_bytes > 0
            && session.segment_bytes.saturating_add(bytes.len() as u64) > config.segment_bytes;
        let record_segment = if rotate {
            session.segment.saturating_add(1)
        } else {
            session.segment
        };
        let record_offset = if rotate { 0 } else { session.segment_bytes };
        let mut index_record = serde_json::to_vec(&serde_json::json!({
            "schema": SCHEMA,
            "session_id": session_id.to_string(),
            "channel": session.manifest.channel,
            "segment": record_segment,
            "record_sequence": session.sequence.saturating_add(1),
            "offset": record_offset,
            "length": bytes.len(),
            "host_time": format_time(wall_time),
            "host_time_unix_ms": wall_time.unix_timestamp_nanos() / 1_000_000,
            "host_t_mono_us": host_t_mono_us
        }))?;
        index_record.push(b'\n');
        let required = (bytes.len() as u64).saturating_add(index_record.len() as u64);
        let mut written_total = archive_bytes.load(Ordering::Relaxed);
        if written_total.saturating_add(required) > config.total_bytes {
            let freed = enforce_retention(
                &config.root,
                config.total_bytes.saturating_sub(required),
                config.retention,
            )?;
            written_total = written_total.saturating_sub(freed);
            archive_bytes.store(written_total, Ordering::Relaxed);
            if written_total.saturating_add(required) > config.total_bytes {
                bail!("serial log quota exhausted; UART forwarding continued");
            }
        }
        if rotate {
            session.raw.flush()?;
            session.index.flush()?;
            session.segment += 1;
            let (raw, index) = open_segment(&session.directory, session.segment)?;
            session.raw = raw;
            session.index = index;
            session.segment_bytes = 0;
            session.manifest.segments = session.segment;
        }
        let offset = session.segment_bytes;
        debug_assert_eq!(session.segment, record_segment);
        debug_assert_eq!(offset, record_offset);
        session.raw.write_all(bytes)?;
        session.sequence += 1;
        session.index.write_all(&index_record)?;
        session.segment_bytes += bytes.len() as u64;
        session.manifest.bytes += bytes.len() as u64;
        session.manifest.records += 1;
        archive_bytes.fetch_add(required, Ordering::Relaxed);
        Ok(())
    })();
    if result.is_err() {
        session.writer_failed = true;
    }
    result
}

fn finish_writer_session(
    sessions: &mut HashMap<Uuid, ActiveSession>,
    session_id: Uuid,
    reason: String,
    ended_at: OffsetDateTime,
    loss: SessionLoss,
) -> Result<()> {
    let Some(mut session) = sessions.remove(&session_id) else {
        return Ok(());
    };
    session.raw.flush()?;
    session.index.flush()?;
    session.manifest.dropped_bytes = loss.dropped_bytes;
    session.manifest.complete = !loss.failed && !session.writer_failed;
    session.manifest.status = if session.manifest.complete {
        "complete"
    } else {
        "incomplete"
    }
    .to_owned();
    session.manifest.ended_at = Some(format_time(ended_at));
    session.manifest.end_reason = Some(reason);
    write_manifest(&session.directory, &session.manifest)
}

fn open_segment(directory: &Path, segment: u32) -> Result<(BufWriter<File>, BufWriter<File>)> {
    let raw = open_private_append(&directory.join(format!("rx-{segment:06}.bin")))?;
    let index = open_private_append(&directory.join(format!("rx-{segment:06}.ndjson")))?;
    Ok((BufWriter::new(raw), BufWriter::new(index)))
}

fn write_manifest(directory: &Path, manifest: &SerialLogManifest) -> Result<()> {
    let path = directory.join("manifest.json");
    let temporary = directory.join("manifest.json.tmp");
    let bytes = serde_json::to_vec_pretty(manifest)?;
    write_private(&temporary, &bytes)?;
    fs::rename(&temporary, &path)?;
    Ok(())
}

fn recover_interrupted(root: &Path) -> Result<()> {
    for mut summary in list_manifests(root)? {
        if summary.manifest.status != "recording" {
            continue;
        }
        summary.manifest.status = "interrupted".to_owned();
        summary.manifest.complete = false;
        refresh_manifest_counts(&root.join(&summary.directory), &mut summary.manifest)?;
        summary.manifest.ended_at = Some(format_time(OffsetDateTime::now_utc()));
        summary.manifest.end_reason =
            Some("host restarted before session was finalized".to_owned());
        write_manifest(&root.join(summary.directory), &summary.manifest)?;
    }
    Ok(())
}

fn refresh_manifest_counts(directory: &Path, manifest: &mut SerialLogManifest) -> Result<()> {
    let raw = matching_paths(directory, "rx-", ".bin")?;
    let indexes = matching_paths(directory, "rx-", ".ndjson")?;
    manifest.bytes = raw.iter().try_fold(0_u64, |total, path| {
        Ok::<_, std::io::Error>(total.saturating_add(fs::metadata(path)?.len()))
    })?;
    manifest.records = indexes.iter().try_fold(0_u64, |total, path| {
        let bytes = fs::read(path)?;
        Ok::<_, std::io::Error>(
            total.saturating_add(bytes.iter().filter(|byte| **byte == b'\n').count() as u64),
        )
    })?;
    manifest.segments = raw.len().max(1).min(u32::MAX as usize) as u32;
    Ok(())
}

fn list_manifests(root: &Path) -> Result<Vec<SerialLogSummary>> {
    let mut results = Vec::new();
    reject_symlink(root)?;
    if !root.is_dir() {
        return Ok(results);
    }
    for date in fs::read_dir(root)? {
        let date = date?;
        if !date.file_type()?.is_dir() {
            continue;
        }
        for session in fs::read_dir(date.path())? {
            let session = session?;
            if !session.file_type()?.is_dir() {
                continue;
            }
            let manifest_path = session.path().join("manifest.json");
            if !fs::symlink_metadata(&manifest_path)
                .is_ok_and(|metadata| metadata.file_type().is_file())
            {
                continue;
            }
            let Ok(bytes) = fs::read(&manifest_path) else {
                continue;
            };
            let Ok(mut manifest) = serde_json::from_slice::<SerialLogManifest>(&bytes) else {
                continue;
            };
            manifest.pinned = fs::symlink_metadata(session.path().join(".pinned"))
                .is_ok_and(|metadata| metadata.file_type().is_file());
            let relative = session
                .path()
                .strip_prefix(root)
                .context("serial log escaped configured root")?
                .to_string_lossy()
                .into_owned();
            results.push(SerialLogSummary {
                manifest,
                directory: relative,
            });
        }
    }
    results.sort_by(|a, b| b.manifest.started_at.cmp(&a.manifest.started_at));
    Ok(results)
}

fn read_matching(directory: &Path, prefix: &str, suffix: &str) -> Result<Vec<u8>> {
    let paths = matching_paths(directory, prefix, suffix)?;
    let mut output = Vec::new();
    for path in paths {
        output.extend_from_slice(&fs::read(path)?);
    }
    Ok(output)
}

fn matching_paths(directory: &Path, prefix: &str, suffix: &str) -> Result<Vec<PathBuf>> {
    let mut paths = fs::read_dir(directory)?
        .filter_map(Result::ok)
        .filter_map(|entry| entry.file_type().ok()?.is_file().then(|| entry.path()))
        .filter(|path| {
            path.file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| name.starts_with(prefix) && name.ends_with(suffix))
        })
        .collect::<Vec<_>>();
    paths.sort();
    Ok(paths)
}

fn directory_size(root: &Path) -> Result<u64> {
    let mut total = 0_u64;
    reject_symlink(root)?;
    if !root.is_dir() {
        return Ok(0);
    }
    for entry in fs::read_dir(root)? {
        let entry = entry?;
        let file_type = entry.file_type()?;
        if file_type.is_dir() {
            total = total.saturating_add(directory_size(&entry.path())?);
        } else if file_type.is_file() {
            total = total.saturating_add(entry.metadata()?.len());
        }
    }
    Ok(total)
}

fn subtract_atomic(value: &AtomicU64, amount: u64) {
    let _ = value.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |current| {
        Some(current.saturating_sub(amount))
    });
}

fn adjust_atomic(value: &AtomicU64, before: u64, after: u64) {
    if after >= before {
        value.fetch_add(after - before, Ordering::Relaxed);
    } else {
        subtract_atomic(value, before - after);
    }
}

fn unpinned_archive_size(root: &Path) -> Result<u64> {
    list_manifests(root)?
        .into_iter()
        .try_fold(0_u64, |total, session| {
            if session.manifest.pinned {
                Ok(total)
            } else {
                Ok(total.saturating_add(directory_size(&root.join(session.directory))?))
            }
        })
}

fn enforce_retention(root: &Path, quota: u64, retention: Duration) -> Result<u64> {
    let now = SystemTime::now();
    let mut sessions = list_manifests(root)?;
    sessions.sort_by(|a, b| a.manifest.started_at.cmp(&b.manifest.started_at));
    let mut total = unpinned_archive_size(root)?;
    let mut freed = 0_u64;
    for session in sessions {
        if session.manifest.status == "recording" || session.manifest.pinned {
            continue;
        }
        let directory = root.join(&session.directory);
        let expired = fs::metadata(&directory)
            .and_then(|metadata| metadata.modified())
            .ok()
            .and_then(|modified| now.duration_since(modified).ok())
            .is_some_and(|age| age > retention);
        if total <= quota && !expired {
            continue;
        }
        let size = directory_size(&directory).unwrap_or_default();
        fs::remove_dir_all(&directory)?;
        total = total.saturating_sub(size);
        freed = freed.saturating_add(size);
    }
    Ok(freed)
}

fn format_time(value: OffsetDateTime) -> String {
    value
        .format(&Iso8601::DEFAULT)
        .unwrap_or_else(|_| value.unix_timestamp().to_string())
}

pub fn default_log_root() -> PathBuf {
    dirs::data_local_dir()
        .unwrap_or_else(std::env::temp_dir)
        .join("radxa-linkr-debugger")
        .join("serial-logs")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(root: PathBuf, segment_bytes: u64) -> SerialLogConfig {
        SerialLogConfig {
            enabled: true,
            root,
            segment_bytes,
            total_bytes: 1024 * 1024,
            retention: Duration::from_secs(30 * 24 * 60 * 60),
            queue_records: 16,
        }
    }

    async fn wait_until(mut condition: impl FnMut() -> bool) {
        for _ in 0..100 {
            if condition() {
                return;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        panic!("condition was not met");
    }

    #[tokio::test]
    async fn preserves_invalid_utf8_and_nul_bytes_and_rotates() {
        let directory = tempfile::tempdir().unwrap();
        let service = SerialLogService::new(config(directory.path().to_owned(), 4))
            .await
            .unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        service.record_rx(id, vec![0x00, 0xff, b'A']);
        service.record_rx(id, vec![b'B', b'C', b'D']);
        service.finish_session(id, "test complete").await;
        wait_until(|| {
            service
                .find(id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;

        let summary = service.find(id).unwrap().unwrap();
        assert_eq!(summary.manifest.bytes, 6);
        assert_eq!(summary.manifest.segments, 2);
        assert!(summary.manifest.complete);
        assert_eq!(
            service.read_artifact(id, "raw").unwrap().0,
            vec![0x00, 0xff, b'A', b'B', b'C', b'D']
        );
        let index = String::from_utf8(service.read_artifact(id, "ndjson").unwrap().0).unwrap();
        assert_eq!(index.lines().count(), 2);
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn archive_permissions_are_hardened_for_new_and_existing_sessions() {
        use std::os::unix::fs::PermissionsExt;

        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("serial-logs");
        fs::create_dir_all(&root).unwrap();
        fs::set_permissions(&root, fs::Permissions::from_mode(0o755)).unwrap();
        let service = SerialLogService::new(config(root.clone(), 1024))
            .await
            .unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        service.record_rx(id, b"secret".to_vec());
        service.finish_session(id, "done").await;
        wait_until(|| {
            service
                .find(id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;
        service.set_pinned(id, true).unwrap();

        assert_eq!(
            fs::metadata(&root).unwrap().permissions().mode() & 0o777,
            0o700
        );
        let session = root.join(service.find(id).unwrap().unwrap().directory);
        let date = session.parent().unwrap();
        assert_eq!(
            fs::metadata(date).unwrap().permissions().mode() & 0o777,
            0o700
        );
        assert_eq!(
            fs::metadata(&session).unwrap().permissions().mode() & 0o777,
            0o700
        );
        for entry in fs::read_dir(&session).unwrap() {
            let metadata = entry.unwrap().metadata().unwrap();
            assert_eq!(metadata.permissions().mode() & 0o777, 0o600);
        }

        service.shutdown().await;
        fs::set_permissions(&root, fs::Permissions::from_mode(0o755)).unwrap();
        fs::set_permissions(date, fs::Permissions::from_mode(0o755)).unwrap();
        fs::set_permissions(&session, fs::Permissions::from_mode(0o755)).unwrap();
        fs::write(session.join("manifest.json.tmp"), b"stale").unwrap();
        for entry in fs::read_dir(&session).unwrap() {
            fs::set_permissions(entry.unwrap().path(), fs::Permissions::from_mode(0o644)).unwrap();
        }
        drop(service);

        let recovered = SerialLogService::new(config(root.clone(), 1024))
            .await
            .unwrap();
        assert_eq!(
            fs::metadata(&root).unwrap().permissions().mode() & 0o777,
            0o700
        );
        assert_eq!(
            fs::metadata(date).unwrap().permissions().mode() & 0o777,
            0o700
        );
        assert_eq!(
            fs::metadata(&session).unwrap().permissions().mode() & 0o777,
            0o700
        );
        for entry in fs::read_dir(session).unwrap() {
            let metadata = entry.unwrap().metadata().unwrap();
            assert_eq!(metadata.permissions().mode() & 0o777, 0o600);
        }
        recovered.shutdown().await;
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn archive_root_symlink_is_rejected_without_chmodding_its_target() {
        use std::os::unix::fs::{symlink, PermissionsExt};

        let directory = tempfile::tempdir().unwrap();
        let outside = directory.path().join("outside");
        let root = directory.path().join("serial-logs");
        fs::create_dir(&outside).unwrap();
        fs::set_permissions(&outside, fs::Permissions::from_mode(0o755)).unwrap();
        symlink(&outside, &root).unwrap();

        let service = SerialLogService::new(config(root, 1024)).await.unwrap();

        assert_eq!(service.status().state, "degraded");
        assert_eq!(
            fs::metadata(&outside).unwrap().permissions().mode() & 0o777,
            0o755
        );
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn archive_date_symlink_is_rejected_without_writing_outside_root() {
        use std::os::unix::fs::symlink;

        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("serial-logs");
        let outside = directory.path().join("outside");
        fs::create_dir(&root).unwrap();
        fs::create_dir(&outside).unwrap();
        symlink(
            &outside,
            root.join(OffsetDateTime::now_utc().date().to_string()),
        )
        .unwrap();
        let service = SerialLogService::new(config(root, 1024)).await.unwrap();

        service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        wait_until(|| {
            service.status().last_error.is_some()
                || fs::read_dir(&outside).unwrap().next().is_some()
        })
        .await;

        assert_eq!(service.status().state, "degraded");
        assert!(fs::read_dir(outside).unwrap().next().is_none());
        service.shutdown().await;
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn archive_download_ignores_symlinked_artifacts() {
        use std::os::unix::fs::symlink;

        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("serial-logs");
        let outside = directory.path().join("outside.bin");
        let service = SerialLogService::new(config(root.clone(), 1024))
            .await
            .unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        service.record_rx(id, b"inside".to_vec());
        service.finish_session(id, "done").await;
        wait_until(|| {
            service
                .find(id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;
        let session = root.join(service.find(id).unwrap().unwrap().directory);
        let raw = matching_paths(&session, "rx-", ".bin").unwrap().remove(0);
        service.shutdown().await;
        drop(service);
        fs::write(&outside, b"outside secret").unwrap();
        fs::remove_file(&raw).unwrap();
        symlink(&outside, &raw).unwrap();

        let recovered = SerialLogService::new(config(root, 1024)).await.unwrap();

        assert!(recovered.read_artifact(id, "raw").unwrap().0.is_empty());
        recovered.shutdown().await;
    }

    #[tokio::test]
    async fn restart_marks_recording_manifest_interrupted() {
        let directory = tempfile::tempdir().unwrap();
        let service = SerialLogService::new(config(directory.path().to_owned(), 1024))
            .await
            .unwrap();
        let id = service
            .start_session("uart1", "/dev/test", 921_600)
            .unwrap();
        wait_until(|| service.find(id).is_ok_and(|entry| entry.is_some())).await;
        drop(service);

        let recovered = SerialLogService::new(config(directory.path().to_owned(), 1024))
            .await
            .unwrap();
        let summary = recovered.find(id).unwrap().unwrap();
        assert_eq!(summary.manifest.status, "interrupted");
        assert!(!summary.manifest.complete);
    }

    #[tokio::test]
    async fn pin_blocks_delete_until_unpinned() {
        let directory = tempfile::tempdir().unwrap();
        let service = SerialLogService::new(config(directory.path().to_owned(), 1024))
            .await
            .unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        service.finish_session(id, "done").await;
        wait_until(|| {
            service
                .find(id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;
        service.set_pinned(id, true).unwrap();
        assert!(service.delete(id).is_err());
        service.set_pinned(id, false).unwrap();
        service.delete(id).unwrap();
        assert!(service.find(id).unwrap().is_none());
    }

    #[tokio::test]
    async fn active_session_cannot_be_pinned() {
        let directory = tempfile::tempdir().unwrap();
        let service = SerialLogService::new(config(directory.path().to_owned(), 1024))
            .await
            .unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        wait_until(|| service.find(id).is_ok_and(|entry| entry.is_some())).await;

        assert!(service.set_pinned(id, true).is_err());
        service.finish_session(id, "done").await;
    }

    #[tokio::test]
    async fn queue_overflow_marks_session_incomplete_without_stopping_producer() {
        let directory = tempfile::tempdir().unwrap();
        let mut test_config = config(directory.path().to_owned(), 1024 * 1024);
        test_config.queue_records = 1;
        let service = SerialLogService::new(test_config).await.unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        wait_until(|| service.find(id).is_ok_and(|entry| entry.is_some())).await;

        for _ in 0..20_000 {
            service.record_rx(id, vec![0x55; 1024]);
        }
        assert!(service.status().dropped_bytes > 0);
        wait_until(|| service.status().queued_records == 0).await;
        service.finish_session(id, "producer completed").await;
        wait_until(|| {
            service
                .find(id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;

        let summary = service.find(id).unwrap().unwrap();
        assert_eq!(summary.manifest.status, "incomplete");
        assert!(!summary.manifest.complete);
        assert!(summary.manifest.dropped_bytes > 0);
    }

    #[tokio::test]
    async fn queue_overflow_only_marks_the_affected_uart_session_incomplete() {
        let directory = tempfile::tempdir().unwrap();
        let mut test_config = config(directory.path().to_owned(), 1024 * 1024);
        test_config.queue_records = 1;
        test_config.total_bytes = 64 * 1024 * 1024;
        let service = SerialLogService::new(test_config).await.unwrap();
        let uart0 = service
            .start_session("uart0", "/dev/test0", 115_200)
            .unwrap();
        wait_until(|| service.find(uart0).is_ok_and(|entry| entry.is_some())).await;
        let uart1 = service
            .start_session("uart1", "/dev/test1", 115_200)
            .unwrap();
        wait_until(|| service.find(uart1).is_ok_and(|entry| entry.is_some())).await;

        for _ in 0..20_000 {
            service.record_rx(uart0, vec![0x55; 1024]);
        }
        assert!(service.status().dropped_bytes > 0);
        wait_until(|| service.status().queued_records == 0).await;
        service.record_rx(uart1, b"uart1 remained lossless".to_vec());
        wait_until(|| service.status().queued_records == 0).await;
        service.finish_session(uart0, "uart0 done").await;
        service.finish_session(uart1, "uart1 done").await;
        wait_until(|| {
            [uart0, uart1].into_iter().all(|id| {
                service.find(id).is_ok_and(|entry| {
                    entry.is_some_and(|entry| entry.manifest.status != "recording")
                })
            })
        })
        .await;

        let uart0 = service.find(uart0).unwrap().unwrap();
        let uart1 = service.find(uart1).unwrap().unwrap();
        assert!(!uart0.manifest.complete);
        assert!(uart0.manifest.dropped_bytes > 0);
        assert!(uart1.manifest.complete);
        assert_eq!(uart1.manifest.dropped_bytes, 0);
    }

    #[tokio::test]
    async fn quota_exhaustion_marks_session_incomplete() {
        let directory = tempfile::tempdir().unwrap();
        let mut test_config = config(directory.path().to_owned(), 1024 * 1024);
        test_config.total_bytes = 10_000;
        let service = SerialLogService::new(test_config).await.unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        wait_until(|| service.find(id).is_ok_and(|entry| entry.is_some())).await;
        service.record_rx(id, vec![0x11; 7000]);
        service.record_rx(id, vec![0x22; 7000]);
        wait_until(|| service.status().queued_records == 0).await;
        service.finish_session(id, "quota test complete").await;
        wait_until(|| {
            service
                .find(id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;

        let summary = service.find(id).unwrap().unwrap();
        assert!(!summary.manifest.complete);
        assert_eq!(summary.manifest.status, "incomplete");
        assert!(summary.manifest.dropped_bytes >= 7000);
        assert!(service.status().last_error.is_some());
    }

    #[tokio::test]
    async fn quota_counts_ndjson_index_bytes() {
        let directory = tempfile::tempdir().unwrap();
        let mut test_config = config(directory.path().to_owned(), 1024 * 1024);
        test_config.total_bytes = 6_000;
        let service = SerialLogService::new(test_config.clone()).await.unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        wait_until(|| service.find(id).is_ok_and(|entry| entry.is_some())).await;

        for _ in 0..30 {
            service.record_rx(id, vec![0x41]);
            wait_until(|| service.status().queued_records == 0).await;
        }
        service
            .finish_session(id, "index quota test complete")
            .await;
        wait_until(|| {
            service
                .find(id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;

        let summary = service.find(id).unwrap().unwrap();
        assert!(!summary.manifest.complete);
        assert!(summary.manifest.records < 30);
        assert_eq!(summary.manifest.bytes, summary.manifest.records);
        assert!(summary.manifest.dropped_bytes > 0);
        assert!(
            directory_size(directory.path()).unwrap() <= test_config.total_bytes + 1024,
            "the final manifest may grow slightly after quota enforcement"
        );
    }

    #[tokio::test]
    async fn active_manifest_reports_live_progress() {
        let directory = tempfile::tempdir().unwrap();
        let service = SerialLogService::new(config(directory.path().to_owned(), 4))
            .await
            .unwrap();
        let id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        wait_until(|| service.find(id).is_ok_and(|entry| entry.is_some())).await;

        service.record_rx(id, b"abc".to_vec());
        service.record_rx(id, b"def".to_vec());
        wait_until(|| {
            service.find(id).is_ok_and(|entry| {
                entry.is_some_and(|entry| {
                    entry.manifest.bytes == 6
                        && entry.manifest.records == 2
                        && entry.manifest.segments == 2
                })
            })
        })
        .await;

        let summary = service.find(id).unwrap().unwrap();
        assert_eq!(summary.manifest.status, "recording");
        assert_eq!(summary.manifest.bytes, 6);
        assert_eq!(summary.manifest.records, 2);
        assert_eq!(summary.manifest.segments, 2);
        service
            .finish_session(id, "live progress test complete")
            .await;
    }

    #[tokio::test]
    async fn pinning_completed_session_releases_quota_for_active_rx() {
        let directory = tempfile::tempdir().unwrap();
        let mut test_config = config(directory.path().to_owned(), 1024 * 1024);
        test_config.total_bytes = 10_000;
        let service = SerialLogService::new(test_config).await.unwrap();

        let old_id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        service.record_rx(old_id, vec![0x33; 4000]);
        service.finish_session(old_id, "old session complete").await;
        wait_until(|| {
            service
                .find(old_id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status == "complete"))
        })
        .await;

        let current_id = service
            .start_session("uart1", "/dev/test", 115_200)
            .unwrap();
        wait_until(|| service.find(current_id).is_ok_and(|entry| entry.is_some())).await;
        service.set_pinned(old_id, true).unwrap();
        service.record_rx(current_id, vec![0x44; 7000]);
        service
            .finish_session(current_id, "current session complete")
            .await;
        wait_until(|| {
            service
                .find(current_id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;

        assert!(service.find(old_id).unwrap().unwrap().manifest.pinned);
        let current = service.find(current_id).unwrap().unwrap();
        assert!(current.manifest.complete);
        assert_eq!(current.manifest.bytes, 7000);
    }

    #[tokio::test]
    async fn deleting_completed_session_releases_quota_for_active_rx() {
        let directory = tempfile::tempdir().unwrap();
        let mut test_config = config(directory.path().to_owned(), 1024 * 1024);
        test_config.total_bytes = 10_000;
        let service = SerialLogService::new(test_config).await.unwrap();

        let old_id = service
            .start_session("uart0", "/dev/test", 115_200)
            .unwrap();
        service.record_rx(old_id, vec![0x33; 4000]);
        service.finish_session(old_id, "old session complete").await;
        wait_until(|| {
            service
                .find(old_id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status == "complete"))
        })
        .await;

        let current_id = service
            .start_session("uart1", "/dev/test", 115_200)
            .unwrap();
        wait_until(|| service.find(current_id).is_ok_and(|entry| entry.is_some())).await;
        service.delete(old_id).unwrap();
        service.record_rx(current_id, vec![0x44; 7000]);
        service
            .finish_session(current_id, "current session complete")
            .await;
        wait_until(|| {
            service
                .find(current_id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;

        assert!(service.find(old_id).unwrap().is_none());
        let current = service.find(current_id).unwrap().unwrap();
        assert!(current.manifest.complete);
        assert_eq!(current.manifest.bytes, 7000);
    }

    #[tokio::test]
    async fn retention_frees_old_session_space_for_current_rx() {
        let directory = tempfile::tempdir().unwrap();
        let mut test_config = config(directory.path().to_owned(), 1024 * 1024);
        test_config.total_bytes = 10_000;

        let first = SerialLogService::new(test_config.clone()).await.unwrap();
        let old_id = first.start_session("uart0", "/dev/test", 115_200).unwrap();
        first.record_rx(old_id, vec![0x33; 4000]);
        first.finish_session(old_id, "old session complete").await;
        wait_until(|| {
            first
                .find(old_id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status == "complete"))
        })
        .await;
        first.shutdown().await;

        let second = SerialLogService::new(test_config).await.unwrap();
        let current_id = second.start_session("uart1", "/dev/test", 115_200).unwrap();
        second.record_rx(current_id, vec![0x44; 7000]);
        second
            .finish_session(current_id, "current session complete")
            .await;
        wait_until(|| {
            second
                .find(current_id)
                .is_ok_and(|entry| entry.is_some_and(|entry| entry.manifest.status != "recording"))
        })
        .await;

        assert!(second.find(old_id).unwrap().is_none());
        let current = second.find(current_id).unwrap().unwrap();
        assert!(current.manifest.complete);
        assert_eq!(current.manifest.bytes, 7000);
        assert_eq!(second.status().dropped_bytes, 0);
    }

    #[tokio::test]
    async fn unavailable_root_degrades_logging_without_failing_host_startup() {
        let directory = tempfile::tempdir().unwrap();
        let blocked_root = directory.path().join("not-a-directory");
        fs::write(&blocked_root, b"occupied by a file").unwrap();
        let service = SerialLogService::new(config(blocked_root, 1024))
            .await
            .unwrap();
        let status = service.status();
        assert!(status.enabled);
        assert_eq!(status.state, "degraded");
        assert!(status.last_error.is_some());
        assert!(service
            .start_session("uart0", "/dev/test", 115_200)
            .is_none());
    }
}
