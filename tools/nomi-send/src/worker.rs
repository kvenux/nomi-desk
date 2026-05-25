use crate::ble::{send_payload, SendOptions};
use crate::state::StatePaths;
use anyhow::Result;
use std::fs::File;
use std::future::Future;
use std::pin::Pin;
use std::time::Duration;

const MAX_LATEST_ITERATIONS: usize = 8;
const WORKER_LOCK_RETRY_ATTEMPTS: usize = 40;
const WORKER_LOCK_RETRY_DELAY: Duration = Duration::from_millis(150);

type SendFuture<'a> = Pin<Box<dyn Future<Output = Result<usize>> + 'a>>;
type SendFn<'a> = dyn FnMut(Vec<u8>) -> SendFuture<'a> + 'a;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum LoopStatus {
    Stable,
    Churned,
}

pub async fn run_once(paths: &StatePaths, options: &SendOptions) -> Result<bool> {
    run_once_with_sender(paths, &mut |payload| {
        Box::pin(async move { send_payload(&payload, options).await })
    })
    .await
}

async fn run_once_with_sender<'a>(paths: &StatePaths, send: &mut SendFn<'a>) -> Result<bool> {
    let Some(_lock) = paths.try_lock_worker()? else {
        paths.append_log("worker already running");
        return Ok(false);
    };

    let status = run_locked_loop_with_sender(paths, || paths.read_latest(), send).await?;
    Ok(status == LoopStatus::Stable)
}

pub async fn run_until_idle(paths: &StatePaths, options: &SendOptions) -> Result<bool> {
    run_until_idle_with_sender(
        paths,
        &mut |payload| Box::pin(async move { send_payload(&payload, options).await }),
        WORKER_LOCK_RETRY_ATTEMPTS,
        WORKER_LOCK_RETRY_DELAY,
        || paths.read_latest(),
    )
    .await
}

pub async fn run_with_lock(paths: &StatePaths, options: &SendOptions, _lock: File) -> Result<bool> {
    run_with_lock_and_sender(
        paths,
        &mut |payload| Box::pin(async move { send_payload(&payload, options).await }),
        || paths.read_latest(),
    )
    .await
}

async fn run_with_lock_and_sender<'a, F>(
    paths: &StatePaths,
    send: &mut SendFn<'a>,
    mut read_latest: F,
) -> Result<bool>
where
    F: FnMut() -> Result<Vec<u8>>,
{
    let first_status = run_locked_loop_with_sender(paths, &mut read_latest, send).await?;
    if first_status == LoopStatus::Stable {
        return Ok(true);
    }

    paths.append_log("worker retrying after latest payload churn");
    let second_status = run_locked_loop_with_sender(paths, &mut read_latest, send).await?;
    Ok(second_status == LoopStatus::Stable)
}

async fn run_until_idle_with_sender<'a, F>(
    paths: &StatePaths,
    send: &mut SendFn<'a>,
    attempts: usize,
    delay: Duration,
    read_latest: F,
) -> Result<bool>
where
    F: FnMut() -> Result<Vec<u8>>,
{
    for attempt in 0..attempts {
        if let Some(_lock) = paths.try_lock_worker()? {
            return run_with_lock_and_sender(paths, send, read_latest).await;
        }

        paths.append_log("worker already running; waiting");
        if attempt + 1 < attempts {
            tokio::time::sleep(delay).await;
        }
    }

    Ok(false)
}

async fn run_locked_loop_with_sender<F>(
    paths: &StatePaths,
    mut read_latest: F,
    send: &mut SendFn<'_>,
) -> Result<LoopStatus>
where
    F: FnMut() -> Result<Vec<u8>>,
{
    let mut payload = read_latest()?;
    paths.append_log(&format!("worker read {} bytes", payload.len()));

    for iteration in 1..MAX_LATEST_ITERATIONS {
        match send(payload.clone()).await {
            Ok(devices) => paths.append_log(&format!("sent payload to {devices} device(s)")),
            Err(err) => paths.append_log(&format!("send failed: {err:#}")),
        }

        let latest = read_latest()?;
        if latest == payload {
            return Ok(LoopStatus::Stable);
        }

        payload = latest;
        paths.append_log(&format!("worker read {} bytes", payload.len()));

        if iteration == MAX_LATEST_ITERATIONS - 1 {
            paths.append_log("worker stopped after latest payload churn");
        }
    }

    Ok(LoopStatus::Churned)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

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

    #[tokio::test]
    async fn run_once_reads_latest_payload_and_logs_bytes() {
        let (_temp, paths) = temp_paths();
        paths.write_latest(b"abc123").unwrap();
        let mut sent = Vec::new();

        assert!(run_once_with_sender(&paths, &mut |payload| {
            sent.push(payload);
            Box::pin(async { Ok(1) })
        })
        .await
        .unwrap());

        let log = fs::read_to_string(paths.log_file).unwrap();
        assert!(log.contains("worker read 6 bytes\n"));
        assert_eq!(sent, vec![b"abc123".to_vec()]);
    }

    #[tokio::test]
    async fn run_once_returns_false_when_worker_lock_is_held() {
        let (_temp, paths) = temp_paths();
        paths.write_latest(b"abc123").unwrap();
        let lock = paths.try_lock_worker().unwrap().unwrap();

        assert!(
            !run_once_with_sender(&paths, &mut |_payload| Box::pin(async { Ok(1) }))
                .await
                .unwrap()
        );

        drop(lock);
    }

    #[tokio::test]
    async fn run_until_idle_waits_for_worker_lock_then_processes_latest() {
        let (_temp, paths) = temp_paths();
        paths.write_latest(b"abc123").unwrap();
        let lock = paths.try_lock_worker().unwrap().unwrap();

        let release = tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(25)).await;
            drop(lock);
        });

        assert!(run_until_idle_with_sender(
            &paths,
            &mut |_payload| Box::pin(async { Ok(1) }),
            10,
            Duration::from_millis(10),
            || paths.read_latest(),
        )
        .await
        .unwrap());
        release.await.unwrap();

        let log = fs::read_to_string(paths.log_file).unwrap();
        assert!(log.contains("worker already running; waiting\n"));
        assert!(log.contains("worker read 6 bytes\n"));
    }

    #[tokio::test]
    async fn run_until_idle_returns_false_when_worker_lock_stays_held() {
        let (_temp, paths) = temp_paths();
        paths.write_latest(b"abc123").unwrap();
        let lock = paths.try_lock_worker().unwrap().unwrap();

        assert!(!run_until_idle_with_sender(
            &paths,
            &mut |_payload| Box::pin(async { Ok(1) }),
            2,
            Duration::from_millis(1),
            || paths.read_latest(),
        )
        .await
        .unwrap());

        drop(lock);
    }

    #[tokio::test]
    async fn run_locked_loop_processes_changed_latest_before_exiting() {
        let (_temp, paths) = temp_paths();
        let reads = [b"one".to_vec(), b"two-two".to_vec(), b"two-two".to_vec()];
        let mut index = 0;

        run_locked_loop_with_sender(
            &paths,
            || {
                let payload = reads[index].clone();
                index += 1;
                Ok(payload)
            },
            &mut |_payload| Box::pin(async { Ok(1) }),
        )
        .await
        .unwrap();

        let log = fs::read_to_string(paths.log_file).unwrap();
        assert!(log.contains("worker read 3 bytes\n"));
        assert!(log.contains("worker read 7 bytes\n"));
    }

    #[tokio::test]
    async fn run_locked_loop_sends_payload_and_logs_success() {
        let (_temp, paths) = temp_paths();
        let mut sent = Vec::new();

        run_locked_loop_with_sender(&paths, || Ok(b"abc123".to_vec()), &mut |payload| {
            sent.push(payload);
            Box::pin(async { Ok(2) })
        })
        .await
        .unwrap();

        assert_eq!(sent, vec![b"abc123".to_vec()]);
        let log = fs::read_to_string(paths.log_file).unwrap();
        assert!(log.contains("worker read 6 bytes\n"));
        assert!(log.contains("sent payload to 2 device(s)\n"));
    }

    #[tokio::test]
    async fn run_locked_loop_logs_send_failure_without_erroring() {
        let (_temp, paths) = temp_paths();

        run_locked_loop_with_sender(&paths, || Ok(b"abc123".to_vec()), &mut |_payload| {
            Box::pin(async { anyhow::bail!("radio unavailable") })
        })
        .await
        .unwrap();

        let log = fs::read_to_string(paths.log_file).unwrap();
        assert!(log.contains("send failed: radio unavailable\n"));
    }

    #[tokio::test]
    async fn run_locked_loop_sends_changed_latest_before_exiting() {
        let (_temp, paths) = temp_paths();
        let reads = [b"one".to_vec(), b"two-two".to_vec(), b"two-two".to_vec()];
        let mut index = 0;
        let mut sent = Vec::new();

        run_locked_loop_with_sender(
            &paths,
            || {
                let payload = reads[index].clone();
                index += 1;
                Ok(payload)
            },
            &mut |payload| {
                sent.push(payload);
                Box::pin(async { Ok(1) })
            },
        )
        .await
        .unwrap();

        assert_eq!(sent, vec![b"one".to_vec(), b"two-two".to_vec()]);
    }

    #[tokio::test]
    async fn run_locked_loop_returns_churned_when_latest_never_stabilizes() {
        let (_temp, paths) = temp_paths();
        let mut index = 0;

        let status = run_locked_loop_with_sender(
            &paths,
            || {
                index += 1;
                Ok(format!("payload-{index}").into_bytes())
            },
            &mut |_payload| Box::pin(async { Ok(1) }),
        )
        .await
        .unwrap();

        assert_eq!(status, LoopStatus::Churned);
        let log = fs::read_to_string(paths.log_file).unwrap();
        assert!(log.contains("worker stopped after latest payload churn\n"));
    }

    #[tokio::test]
    async fn run_until_idle_retries_one_more_bounded_pass_after_churn() {
        let (_temp, paths) = temp_paths();
        let mut index = 0;

        assert!(run_until_idle_with_sender(
            &paths,
            &mut |_payload| Box::pin(async { Ok(1) }),
            1,
            Duration::from_millis(1),
            || {
                index += 1;
                let payload = if index < MAX_LATEST_ITERATIONS + 1 {
                    format!("churn-{index}").into_bytes()
                } else {
                    b"stable".to_vec()
                };
                Ok::<Vec<u8>, anyhow::Error>(payload)
            },
        )
        .await
        .unwrap());

        let log = fs::read_to_string(paths.log_file).unwrap();
        assert!(log.contains("worker stopped after latest payload churn\n"));
        assert!(log.contains("worker retrying after latest payload churn\n"));
    }

    #[tokio::test]
    async fn run_with_lock_processes_payload_without_retry_wait() {
        let (_temp, paths) = temp_paths();
        let lock = paths.try_lock_worker().unwrap().unwrap();
        let mut sent = Vec::new();

        assert!(run_with_lock_and_sender(
            &paths,
            &mut |payload| {
                sent.push(payload);
                Box::pin(async { Ok(1) })
            },
            || Ok(b"locked".to_vec()),
        )
        .await
        .unwrap());

        drop(lock);
        assert_eq!(sent, vec![b"locked".to_vec()]);
    }

    #[tokio::test]
    async fn run_until_idle_returns_false_when_retry_pass_also_churns() {
        let (_temp, paths) = temp_paths();
        let mut index = 0;

        assert!(!run_until_idle_with_sender(
            &paths,
            &mut |_payload| Box::pin(async { Ok(1) }),
            1,
            Duration::from_millis(1),
            || {
                index += 1;
                Ok::<Vec<u8>, anyhow::Error>(format!("payload-{index}").into_bytes())
            },
        )
        .await
        .unwrap());
    }
}
