use anyhow::{bail, Result};
use serde::{Deserialize, Serialize};

pub const PROTOCOL: &str = "nomi-agent-display";
pub const VERSION: u8 = 1;

pub const SERVICE_UUID: &str = "f4f688c2-613e-56a5-b115-d19a99d1b463";
pub const RX_UUID: &str = "74879a99-7275-5b33-9665-51519f328fa5";
pub const TX_UUID: &str = "830ac719-8dea-541c-8d18-5e8de4cd83dd";
pub const INFO_UUID: &str = "485d9275-a3ad-516d-a524-e284f0aafdb1";
pub const MAX_ENCODED_PAYLOAD_BYTES: usize = 512;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Quota {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub five_hour_left: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub weekly_left: Option<u8>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum NomiState {
    Offline,
    Idle,
    Active,
    WaitingApproval,
    WaitingInput,
    Failed,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NomiPayload {
    pub protocol: String,
    pub version: u8,
    pub source: String,
    pub state: NomiState,
    pub time: String,
    pub event: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub model: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub effort: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tier: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub session: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub prompt: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub context_pct: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub used_tokens_k: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub quota: Option<Quota>,
}

impl NomiPayload {
    pub fn validate(&self) -> Result<()> {
        if self.protocol != PROTOCOL {
            bail!("invalid protocol: {}", self.protocol);
        }
        if self.version != VERSION {
            bail!("invalid version: {}", self.version);
        }
        if self.source.trim().is_empty() {
            bail!("source is required");
        }
        if self.time.trim().is_empty() {
            bail!("time is required");
        }
        if self.event.trim().is_empty() {
            bail!("event is required");
        }
        if self
            .context_pct
            .is_some_and(|context_pct| context_pct > 100)
        {
            bail!("context_pct must be <= 100");
        }
        Ok(())
    }
}

pub fn encode_payload(payload: &NomiPayload) -> Result<Vec<u8>> {
    payload.validate()?;
    let bytes = serde_json::to_vec(payload)?;
    if bytes.len() > MAX_ENCODED_PAYLOAD_BYTES {
        bail!(
            "encoded payload is {} bytes; max is {}",
            bytes.len(),
            MAX_ENCODED_PAYLOAD_BYTES
        );
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_minimal_payload() {
        let payload = NomiPayload {
            protocol: PROTOCOL.to_string(),
            version: VERSION,
            source: "codex".to_string(),
            state: NomiState::Active,
            time: "14:25".to_string(),
            event: "UserPromptSubmit".to_string(),
            model: None,
            effort: None,
            tier: None,
            session: None,
            prompt: None,
            context_pct: None,
            used_tokens_k: None,
            quota: None,
        };
        assert!(payload.validate().is_ok());
        let json = String::from_utf8(encode_payload(&payload).unwrap()).unwrap();
        assert!(json.contains("\"protocol\":\"nomi-agent-display\""));
        assert!(json.contains("\"state\":\"active\""));
    }

    #[test]
    fn preserves_model_effort_tier_and_window_snapshot() {
        let raw = r#"{
            "protocol":"nomi-agent-display",
            "version":1,
            "source":"codex",
            "state":"active",
            "time":"14:25",
            "event":"UserPromptSubmit",
            "model":"gpt-5.5",
            "effort":"medium",
            "tier":"default",
            "context_pct":83,
            "used_tokens_k":215,
            "quota":{"five_hour_left":82,"weekly_left":78}
        }"#;

        let payload: NomiPayload = serde_json::from_str(raw).unwrap();

        assert_eq!(payload.model.as_deref(), Some("gpt-5.5"));
        assert_eq!(payload.effort.as_deref(), Some("medium"));
        assert_eq!(payload.tier.as_deref(), Some("default"));
        assert_eq!(payload.context_pct, Some(83));
        assert_eq!(payload.used_tokens_k, Some(215));
        assert_eq!(
            payload.quota,
            Some(Quota {
                five_hour_left: Some(82),
                weekly_left: Some(78)
            })
        );

        let json = String::from_utf8(encode_payload(&payload).unwrap()).unwrap();
        assert!(json.contains("\"effort\":\"medium\""));
        assert!(json.contains("\"tier\":\"default\""));
    }

    #[test]
    fn rejects_unknown_state() {
        let raw = r#"{
            "protocol":"nomi-agent-display",
            "version":1,
            "source":"codex",
            "state":"busy",
            "time":"14:25",
            "event":"UserPromptSubmit"
        }"#;
        assert!(serde_json::from_str::<NomiPayload>(raw).is_err());
    }

    #[test]
    fn rejects_context_pct_over_100() {
        let payload = NomiPayload {
            protocol: PROTOCOL.to_string(),
            version: VERSION,
            source: "codex".to_string(),
            state: NomiState::Active,
            time: "14:25".to_string(),
            event: "UserPromptSubmit".to_string(),
            model: None,
            effort: None,
            tier: None,
            session: None,
            prompt: None,
            context_pct: Some(101),
            used_tokens_k: None,
            quota: None,
        };
        assert!(payload.validate().is_err());
    }

    #[test]
    fn rejects_oversized_encoded_payload() {
        let payload = NomiPayload {
            protocol: PROTOCOL.to_string(),
            version: VERSION,
            source: "codex".to_string(),
            state: NomiState::Active,
            time: "14:25".to_string(),
            event: "UserPromptSubmit".to_string(),
            model: None,
            effort: None,
            tier: None,
            session: None,
            prompt: Some("x".repeat(MAX_ENCODED_PAYLOAD_BYTES)),
            context_pct: None,
            used_tokens_k: None,
            quota: None,
        };

        assert!(encode_payload(&payload).is_err());
    }
}
