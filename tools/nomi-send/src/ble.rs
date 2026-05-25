use crate::protocol::{RX_UUID, SERVICE_UUID};
use anyhow::{anyhow, bail, Context, Result};
use btleplug::api::{Central, CharPropFlags, Manager as _, Peripheral as _, ScanFilter, WriteType};
use btleplug::platform::{Manager, Peripheral};
use std::time::Duration;
use tokio::time::timeout;
use uuid::Uuid;

#[derive(Debug, Clone)]
pub struct SendOptions {
    pub explicit_addresses: Vec<String>,
    pub scan_timeout: Duration,
    pub connect_timeout: Duration,
}

impl Default for SendOptions {
    fn default() -> Self {
        Self {
            explicit_addresses: Vec::new(),
            scan_timeout: Duration::from_secs(4),
            connect_timeout: Duration::from_secs(5),
        }
    }
}

pub async fn send_payload(payload: &[u8], options: &SendOptions) -> Result<usize> {
    let service_uuid = Uuid::parse_str(SERVICE_UUID).context("parse Nomi service UUID")?;
    let rx_uuid = Uuid::parse_str(RX_UUID).context("parse Nomi RX UUID")?;

    let manager = Manager::new().await.context("create BLE manager")?;
    let adapter = manager
        .adapters()
        .await
        .context("list BLE adapters")?
        .into_iter()
        .next()
        .ok_or_else(|| anyhow!("no BLE adapters found"))?;

    adapter
        .start_scan(ScanFilter::default())
        .await
        .context("start BLE scan")?;
    tokio::time::sleep(options.scan_timeout).await;
    let _ = adapter.stop_scan().await;

    let peripherals = adapter
        .peripherals()
        .await
        .context("list BLE peripherals")?;
    let targets = select_targets(peripherals, options, service_uuid).await?;
    if targets.is_empty() {
        if options.explicit_addresses.is_empty() {
            bail!("no Nomi BLE devices found");
        }
        bail!("no explicit BLE addresses matched discovered peripherals");
    }

    let mut successes = 0;
    let mut failures = Vec::new();
    for peripheral in targets {
        match send_to_peripheral(&peripheral, payload, rx_uuid, options.connect_timeout).await {
            Ok(()) => successes += 1,
            Err(err) => failures.push(format!("{err:#}")),
        }
        disconnect_best_effort(&peripheral, options.connect_timeout).await;
    }

    if successes == 0 {
        if failures.is_empty() {
            bail!("zero devices accepted payload");
        }
        bail!("zero devices accepted payload: {}", failures.join("; "));
    }

    Ok(successes)
}

async fn select_targets(
    peripherals: Vec<Peripheral>,
    options: &SendOptions,
    service_uuid: Uuid,
) -> Result<Vec<Peripheral>> {
    if !options.explicit_addresses.is_empty() {
        let addresses: Vec<_> = options
            .explicit_addresses
            .iter()
            .map(|address| address.to_ascii_lowercase())
            .collect();

        return Ok(peripherals
            .into_iter()
            .filter(|peripheral| {
                let discovered = peripheral.address().to_string().to_ascii_lowercase();
                addresses.iter().any(|address| address == &discovered)
            })
            .collect());
    }

    let mut targets = Vec::new();
    for peripheral in peripherals {
        let Some(properties) = peripheral
            .properties()
            .await
            .with_context(|| format!("read properties for {}", peripheral.address()))?
        else {
            continue;
        };

        let advertises_service = properties.services.iter().any(|uuid| uuid == &service_uuid)
            || properties.service_data.contains_key(&service_uuid);
        let has_nomi_name = properties
            .local_name
            .as_deref()
            .is_some_and(|name| name.starts_with("Nomi "));

        if advertises_service || has_nomi_name {
            targets.push(peripheral);
        }
    }

    Ok(targets)
}

async fn send_to_peripheral(
    peripheral: &Peripheral,
    payload: &[u8],
    rx_uuid: Uuid,
    connect_timeout: Duration,
) -> Result<()> {
    timeout(connect_timeout, peripheral.connect())
        .await
        .context("connect timed out")?
        .with_context(|| format!("connect to {}", peripheral.address()))?;

    timeout(connect_timeout, peripheral.discover_services())
        .await
        .context("discover services timed out")?
        .with_context(|| format!("discover services on {}", peripheral.address()))?;

    let characteristic = peripheral
        .characteristics()
        .into_iter()
        .find(|characteristic| characteristic.uuid == rx_uuid)
        .ok_or_else(|| anyhow!("RX characteristic not found on {}", peripheral.address()))?;
    let write_type = choose_write_type(characteristic.properties)?;

    timeout(
        connect_timeout,
        peripheral.write(&characteristic, payload, write_type),
    )
    .await
    .context("write payload timed out")?
    .with_context(|| format!("write payload to {}", peripheral.address()))?;

    Ok(())
}

fn choose_write_type(properties: CharPropFlags) -> Result<WriteType> {
    if properties.contains(CharPropFlags::WRITE) {
        Ok(WriteType::WithResponse)
    } else if properties.contains(CharPropFlags::WRITE_WITHOUT_RESPONSE) {
        Ok(WriteType::WithoutResponse)
    } else {
        bail!("RX characteristic is not writable");
    }
}

async fn disconnect_best_effort(peripheral: &Peripheral, operation_timeout: Duration) {
    let _ = timeout(operation_timeout, peripheral.disconnect()).await;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn choose_write_type_prefers_response_when_supported() {
        assert_eq!(
            choose_write_type(CharPropFlags::WRITE | CharPropFlags::WRITE_WITHOUT_RESPONSE)
                .unwrap(),
            WriteType::WithResponse
        );
    }

    #[test]
    fn choose_write_type_uses_without_response_when_only_supported_write() {
        assert_eq!(
            choose_write_type(CharPropFlags::WRITE_WITHOUT_RESPONSE).unwrap(),
            WriteType::WithoutResponse
        );
    }

    #[test]
    fn choose_write_type_errors_when_characteristic_is_not_writable() {
        let error = choose_write_type(CharPropFlags::READ).unwrap_err();

        assert!(error
            .to_string()
            .contains("RX characteristic is not writable"));
    }
}
