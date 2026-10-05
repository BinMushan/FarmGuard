# Cloud backend (planned)

Receives telemetry from the ESP32 (HTTP POST JSON) and returns an optional
`{"cloudVerdict":"NECESSARY|RISKY|UNNECESSARY"}`.

Planned modules: weather API client, crop and cost database, decision engine,
waste estimator, LLM explanation, WhatsApp and dashboard notifications.
