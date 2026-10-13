# Push from Web

> [Reference](../index.md) » **Push from Web**

*Send to UE* on a material's page in the DreamTexture web UI imports that material into a running
Unreal Editor. It works the same as clicking *Import* in the Library, without switching windows.
The backend's MCP interface has a tool of the same name (`send_to_ue`), so an agent can do it too.

It needs a backend with the editor session endpoints; see
[Backend](../backend.md#which-backend-version).

## Using it

1. Open the editor with the plugin enabled and a project whose *Backend URL* points at the backend.
2. In the web UI, open a material and click *Send to UE*.
3. The editor shows a notification that a material arrived and is being imported, and another when the
   import finishes. The material goes through the same import path as everything else: textures, instance,
   and nothing is downloaded twice.

Which editor receives it:

- One editor online: it goes there.
- Several online: the web UI asks which. Each shows the project name and engine version.
- None online: the web UI says so (HTTP 409). Nothing is queued.

The **Connection Status** page's *Push* row shows whether this editor is registered, and under what session name.

## How it works

Each editor registers a **session** with the backend, using REST only. The event stream stays receive-only.

| Step | Detail |
| :-- | :-- |
| Register | After connecting, `POST /api/ue/sessions` with the project name, engine version and process id. The project name is what the web UI lists. |
| Heartbeat | Every 15 seconds. The backend considers a session offline after 45 seconds without one. If the session is gone (HTTP 404), the plugin registers again. |
| Unregister | When the editor closes, best effort. The URL registered with is cached, so closing does not depend on settings. |
| Push | The backend publishes a `ue.import` event on the event stream with the session id, the material id and a `delivery_id`. Every subscriber receives it; the plugin acts only on its own session id. |
| Redelivery | The heartbeat reply (HTTP 200) lists deliveries sent to this session that were not acknowledged (the backend keeps them for 10 minutes). The plugin imports each `delivery_id` once, whether it arrived over the stream or in a heartbeat reply, and acknowledges it in the next heartbeat. |

The redelivery exists because the event stream alone can lose a push: while the editor's stream is in its
reconnect backoff (up to 15 seconds) the session is still alive for 45 seconds, so the web UI would
report success for an event nobody received. With redelivery, the push is applied after the stream or the next
heartbeat catches up, and only once. The web UI's message says the same: "sent" does not mean "already
imported", and an editor that is reconnecting imports it afterwards.

A material already in the import queue is not queued twice.

## Not working?

See [Troubleshooting](../troubleshooting.md#push-from-the-web-ui-does-not-arrive).
