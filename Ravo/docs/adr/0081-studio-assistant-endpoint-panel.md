# ADR-0081: Typed assistant endpoint settings and a floating Studio panel

- Status: Partially superseded by
  [ADR-0090](0090-versioned-live-studio-control.md)
- Date: 2026-08-29
- Extends: [ADR-0066](0066-typed-desktop-language-setting.md)
- Relates to: [ADR-0003](0003-versioned-machine-contract.md)

## Context

Studio needed a floating assistant surface comparable to Audio2Text's Cloud
Model settings: a user-owned HTTP endpoint and model, not a second in-process
control plane and not an MCP wrapper around `ravo`. ADR-0066 allowed only the
typed language preference; adding durable assistant fields requires an explicit
owner, validation, and failure policy.

## Decision

- `StudioAssistantController` owns three typed desktop settings: HTTPS/HTTP
  endpoint URL, model identifier, and API key. Keys are
  `desktop/assistant/endpoint`, `desktop/assistant/model`, and
  `desktop/assistant/api_key`. Defaults are `https://api.x.ai/v1` and
  `grok-4.5`.
- Invalid set requests fail without changing the active or stored value.
  Malformed stored URL or model is removed synchronously and replaced with the
  default; persistence failure is visible.
- An empty stored API key may be filled at send time from `XAI_API_KEY`. The
  key is never written to logs or error context.
- QML presents Settings fields and a non-modal Overlay popup. Chat send,
  cancel, JSON, and HTTP stay in desktop C++. ADR-0090 separately permits Qt
  Network in `ravo_control` and CLI only for the same-user local socket; it does
  not expose this assistant's network client or credentials.
- The session transcript is not persisted. The catalog CLI remains the machine
  contract; this panel does not drive Studio commands.

## Consequences

Photographers can open a floating Assistant from View or `Ctrl/Cmd+Shift+A`
and point it at an OpenAI-compatible endpoint. Engine, catalog, and CLI stay
free of Network and of assistant state.

## Rejected alternatives

- Hard-coding a single vendor SDK with no URL/model settings. That cannot
  match the Audio2Text configuration surface.
- QML `XMLHttpRequest` / fetch. Network, secrets, and JSON belong in C++.
- An MCP-only or unversioned socket into the live Qt process. ADR-0090 later
  accepts a versioned, CLI-first, revision-checked local control contract that
  contains no assistant state.
