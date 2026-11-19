# AI Provider Adapters — Initial Chat Completion Support

## Verified endpoints

The adapters use these official OpenAI-compatible Chat Completions endpoints:

| Provider | Default base URL | Request URL |
|---|---|---|
| OpenAI | `https://api.openai.com/v1` | `POST /chat/completions` |
| DeepSeek | `https://api.deepseek.com` | `POST /chat/completions` |
| Moonshot/Kimi | `https://api.moonshot.ai/v1` | `POST /chat/completions` |

Primary references checked on 2026-11-19:

- OpenAI [Chat Completions](https://developers.openai.com/api/reference/resources/chat/completions/methods/create), [authentication](https://platform.openai.com/docs/api-reference/authentication), and [official OpenAPI schema](https://github.com/openai/openai-openapi/blob/16cdf739aee9baa6191cdbab945dd5f7f9920f20/openapi.yaml).
- DeepSeek [quickstart](https://api-docs.deepseek.com/), [chat completion API](https://api-docs.deepseek.com/api/create-chat-completion/), [rate limits](https://api-docs.deepseek.com/quick_start/rate_limit), and [error codes](https://api-docs.deepseek.com/quick_start/error_codes).
- Moonshot/Kimi [API overview](https://platform.kimi.ai/docs/api/overview.md), [chat completion API](https://platform.kimi.ai/docs/api/chat.md), [tool calling](https://platform.kimi.ai/docs/guide/use-kimi-api-to-complete-tool-calls.md), [errors](https://platform.kimi.ai/docs/api/errors.md), and [rate limits](https://platform.kimi.ai/docs/pricing/limits.md).

Model names are supplied in configuration; the project does not hard-code model
availability or availability dates.

## Implemented behavior

`system/ai/providers.cpp` implements separate OpenAI, DeepSeek, and Kimi adapter
classes over the documented Chat Completions request/response shape. It
supports text messages, JSON-schema tool definitions, returned function
tool-call parsing, usage counts, a configured maximum completion-token cap,
bounded non-streaming response reads,
request timeouts, redacted HTTP/network error categories, and bounded
per-provider concurrent calls.
The Provider pool registry uses a configured instance key rather than only the
vendor name, so multiple separately configured models for one adapter can be
registered and scheduled independently (for example `openai:model-a` and
`openai:model-b`). Each instance keeps its own model, token cap, endpoint,
credential name, and concurrency slot.

`429` and selected `5xx`/network failures are marked retryable and `Retry-After`
is surfaced when available. The adapter does **not** retry automatically.
Redirects are rejected to prevent forwarding credentials to a different
endpoint.

Each request sends `max_tokens`, defaulting to the model's configured
`max_completion_tokens` (2048 by default). A request cannot raise that limit.
When the provider returns usage counts, the scheduler persists prompt and
completion token usage on the task record. An optional per-Agent USD budget
uses administrator-configured input/output price per million tokens to reserve
an estimated maximum cost before dispatch, then settles against returned token
usage. The estimate uses serialized prompt bytes as a token proxy and the
configured completion-token cap; it is not an invoice-level guarantee.
Provider billing rules, tokenization, caching, and price changes can make
actual charges differ. Missing usage or a failure/cancellation after dispatch
is charged at the reserved estimate; incomplete tasks with an active
reservation are settled at that estimate during restart recovery. Without
configured prices, the scheduler refuses to dispatch work for an Agent with a
configured budget.
An optional cumulative per-Agent Provider request quota is enforced in SQLite
before dispatch. It defaults to unlimited and does not reset automatically.

API keys are read only from named files beneath systemd's
`CREDENTIALS_DIRECTORY`, such as `openai-api-key`; keys are not stored in the
Agent database, source code, request logs, or exception messages. Credentials
must be regular files owned by root or the service user, have no group/other
permissions, and be no larger than 4096 bytes. No systemd service unit or
`LoadCredential=` deployment has been installed yet.

## Limits

- Streaming/SSE, provider model listing, provider health checks, and automatic
  retries are not implemented. USD budget enforcement is estimate-based, not a
  hard cap on provider invoices.
- `ProviderPool::complete_many` runs C++ asynchronous requests with a
  per-provider concurrency limit. Scheduler cancellation interrupts an
  in-flight libcurl transfer as well as a request waiting for provider capacity.
- HTTP tests use a local mock server only. No API keys were available or used,
  and no live provider call was made.
- Custom endpoint URLs are accepted for administrators and tests; HTTPS is
  required except for loopback HTTP test URLs.
- Tool calls are returned as data only. This module never executes a tool or
  shell command.
