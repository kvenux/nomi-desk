pub mod ble;
pub mod protocol;
pub mod state;
pub mod worker;

use anyhow::{bail, Result};
use clap::{Parser, Subcommand};
use protocol::{encode_payload, NomiPayload};
use state::StatePaths;
use std::fs;
use std::path::PathBuf;
use std::time::Duration;

#[derive(Debug, Parser)]
#[command(name = "nomi-send")]
#[command(about = "Send Nomi agent display status payloads")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    Validate {
        #[arg(long)]
        json_file: PathBuf,
    },
    Enqueue {
        #[arg(long)]
        json_file: PathBuf,
        #[arg(long)]
        no_spawn: bool,
    },
    Worker {
        #[arg(long = "address")]
        addresses: Vec<String>,
        #[arg(long, default_value_t = 4)]
        scan_timeout_secs: u64,
        #[arg(long, default_value_t = 5)]
        connect_timeout_secs: u64,
    },
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Command::Validate { json_file } => {
            let raw = fs::read_to_string(json_file)?;
            let payload: NomiPayload = serde_json::from_str(&raw)?;
            let bytes = encode_payload(&payload)?;
            println!("valid {} bytes", bytes.len());
            Ok(())
        }
        Command::Enqueue {
            json_file,
            no_spawn,
        } => {
            let raw = fs::read_to_string(&json_file)?;
            let payload: NomiPayload = serde_json::from_str(&raw)?;
            let bytes = encode_payload(&payload)?;
            let paths = StatePaths::discover()?;
            paths.write_latest(&bytes)?;
            paths.append_log(&format!("enqueued {} bytes", bytes.len()));
            let _ = fs::remove_file(&json_file);

            if !no_spawn {
                if let Some(lock) = paths.try_lock_worker()? {
                    if !worker::run_with_lock(&paths, &ble::SendOptions::default(), lock).await? {
                        paths.append_log("worker did not reach idle state");
                    }
                } else if let Some(_follower) = paths.try_lock_follower()? {
                    paths.append_log("worker already running; follower waiting");
                    if !worker::run_until_idle(&paths, &ble::SendOptions::default()).await? {
                        paths.append_log("follower did not reach idle state");
                    }
                } else {
                    paths.append_log(
                        "worker and follower already running; enqueue will be picked up",
                    );
                }
            }

            Ok(())
        }
        Command::Worker {
            addresses,
            scan_timeout_secs,
            connect_timeout_secs,
        } => {
            let paths = StatePaths::discover()?;
            let options = ble::SendOptions {
                explicit_addresses: addresses,
                scan_timeout: Duration::from_secs(scan_timeout_secs),
                connect_timeout: Duration::from_secs(connect_timeout_secs),
            };
            if !worker::run_until_idle(&paths, &options).await? {
                eprintln!("worker did not reach idle state");
                bail!("worker did not reach idle state");
            }
            Ok(())
        }
    }
}
