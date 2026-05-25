use anyhow::{Context, Result};
use directories::ProjectDirs;
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use std::fs::{self, File, OpenOptions};
use std::io::{ErrorKind, Write};
use std::path::PathBuf;

pub struct StatePaths {
    pub root: PathBuf,
    pub latest_payload: PathBuf,
    pub worker_lock: PathBuf,
    pub follower_lock: PathBuf,
    pub address_cache: PathBuf,
    pub log_file: PathBuf,
}

#[derive(Debug, Serialize, Deserialize, Default)]
pub struct AddressCache {
    pub addresses: Vec<String>,
}

impl StatePaths {
    pub fn discover() -> Result<Self> {
        let project_dirs = ProjectDirs::from("dev", "mynomi", "Nomi")
            .context("could not determine Nomi state directory")?;
        let root = project_dirs.data_local_dir().to_path_buf();
        fs::create_dir_all(&root)
            .with_context(|| format!("create state directory {}", root.display()))?;

        Ok(Self {
            latest_payload: root.join("latest.json"),
            worker_lock: root.join("worker.lock"),
            follower_lock: root.join("follower.lock"),
            address_cache: root.join("addresses.json"),
            log_file: root.join("nomi-send.log"),
            root,
        })
    }

    pub fn write_latest(&self, payload: &[u8]) -> Result<()> {
        let _lock = self.lock_latest()?;
        let tmp = self.unique_latest_temp_path();
        fs::write(&tmp, payload).with_context(|| format!("write {}", tmp.display()))?;

        match fs::remove_file(&self.latest_payload) {
            Ok(()) => {}
            Err(err) if err.kind() == ErrorKind::NotFound => {}
            Err(err) => {
                return Err(err)
                    .with_context(|| format!("remove existing {}", self.latest_payload.display()));
            }
        }

        fs::rename(&tmp, &self.latest_payload).with_context(|| {
            format!(
                "rename {} to {}",
                tmp.display(),
                self.latest_payload.display()
            )
        })?;
        Ok(())
    }

    pub fn read_latest(&self) -> Result<Vec<u8>> {
        let _lock = self.lock_latest()?;
        fs::read(&self.latest_payload)
            .with_context(|| format!("read {}", self.latest_payload.display()))
    }

    pub fn try_lock_worker(&self) -> Result<Option<File>> {
        self.try_lock_file(&self.worker_lock)
    }

    pub fn try_lock_follower(&self) -> Result<Option<File>> {
        self.try_lock_file(&self.follower_lock)
    }

    fn try_lock_file(&self, path: &PathBuf) -> Result<Option<File>> {
        let file = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(path)
            .with_context(|| format!("open {}", path.display()))?;

        match file.try_lock_exclusive() {
            Ok(()) => Ok(Some(file)),
            Err(err) if is_lock_contention(&err) => Ok(None),
            Err(err) => Err(err).with_context(|| format!("lock {}", path.display())),
        }
    }

    pub fn is_worker_running(&self) -> Result<bool> {
        let Some(lock) = self.try_lock_worker()? else {
            return Ok(true);
        };

        drop(lock);
        Ok(false)
    }

    pub fn append_log(&self, message: &str) {
        if let Ok(mut file) = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.log_file)
        {
            let _ = writeln!(file, "{message}");
        }
    }

    fn lock_latest(&self) -> Result<File> {
        let file = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(self.latest_lock_path())
            .with_context(|| format!("open latest lock in {}", self.root.display()))?;
        file.lock_exclusive()
            .with_context(|| format!("lock {}", self.latest_lock_path().display()))?;
        Ok(file)
    }

    fn latest_lock_path(&self) -> PathBuf {
        self.root.join("latest.lock")
    }

    fn unique_latest_temp_path(&self) -> PathBuf {
        let thread_id = format!("{:?}", std::thread::current().id());
        let thread_id = sanitize_path_component(&thread_id);
        self.root
            .join(format!("latest.{}.{}.tmp", std::process::id(), thread_id))
    }
}

fn is_lock_contention(err: &std::io::Error) -> bool {
    err.kind() == ErrorKind::WouldBlock || err.raw_os_error() == Some(33)
}

fn sanitize_path_component(value: &str) -> String {
    value
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
                ch
            } else {
                '_'
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_paths() -> (tempfile::TempDir, StatePaths) {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().to_path_buf();
        let paths = StatePaths {
            latest_payload: root.join("latest.json"),
            worker_lock: root.join("worker.lock"),
            follower_lock: root.join("follower.lock"),
            address_cache: root.join("addresses.json"),
            log_file: root.join("nomi-send.log"),
            root,
        };
        (temp, paths)
    }

    #[test]
    fn write_latest_replaces_payload_and_read_latest_returns_it() {
        let (_temp, paths) = temp_paths();

        paths.write_latest(br#"{"event":"first"}"#).unwrap();
        paths.write_latest(br#"{"event":"second"}"#).unwrap();

        assert_eq!(paths.read_latest().unwrap(), br#"{"event":"second"}"#);
        assert!(!paths.latest_payload.with_extension("json.tmp").exists());
    }

    #[test]
    fn write_latest_overwrites_existing_payload_on_windows() {
        let (_temp, paths) = temp_paths();
        std::fs::write(&paths.latest_payload, b"existing").unwrap();

        paths.write_latest(b"replacement").unwrap();

        assert_eq!(
            std::fs::read(&paths.latest_payload).unwrap(),
            b"replacement"
        );
    }

    #[test]
    fn write_latest_uses_unique_temp_paths_and_leaves_no_shared_tmp() {
        let (_temp, paths) = temp_paths();

        paths.write_latest(b"first").unwrap();
        paths.write_latest(b"second").unwrap();

        assert!(!paths.root.join("latest.json.tmp").exists());
        let tmp_entries: Vec<_> = std::fs::read_dir(&paths.root)
            .unwrap()
            .filter_map(Result::ok)
            .filter(|entry| entry.file_name().to_string_lossy().contains(".tmp"))
            .collect();
        assert!(tmp_entries.is_empty());
    }

    #[test]
    fn try_lock_worker_returns_none_when_lock_is_held() {
        let (_temp, paths) = temp_paths();
        let first = paths.try_lock_worker().unwrap().unwrap();

        assert!(paths.try_lock_worker().unwrap().is_none());

        drop(first);
    }

    #[test]
    fn is_worker_running_reflects_lock_state() {
        let (_temp, paths) = temp_paths();

        assert!(!paths.is_worker_running().unwrap());
        let lock = paths.try_lock_worker().unwrap().unwrap();
        assert!(paths.is_worker_running().unwrap());

        drop(lock);
        assert!(!paths.is_worker_running().unwrap());
    }

    #[test]
    fn try_lock_follower_returns_none_when_lock_is_held() {
        let (_temp, paths) = temp_paths();
        let first = paths.try_lock_follower().unwrap().unwrap();

        assert!(paths.try_lock_follower().unwrap().is_none());

        drop(first);
        assert!(paths.try_lock_follower().unwrap().is_some());
    }

    #[test]
    fn append_log_is_best_effort_and_appends_lines() {
        let (_temp, paths) = temp_paths();

        paths.append_log("first");
        paths.append_log("second");

        let log = std::fs::read_to_string(paths.log_file).unwrap();
        assert!(log.contains("first\n"));
        assert!(log.contains("second\n"));
    }
}
