## Summary

<!-- What changed, and why? One or two sentences is enough. -->

## Related issues

<!-- "Fixes #59", "Refs #59", or "None". -->

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Documentation
- [ ] Refactor / code health
- [ ] Build, CI or tooling
- [ ] Other

## Testing

<!-- How did you verify it? "CI build only" is an honest answer; for
     anything on a real console, note the model, firmware and payload
     loader, and what worked or failed. -->

## Checklist

- [ ] The pull request targets `dev`.
- [ ] The payload still builds: `make -C payload PS5_PAYLOAD_SDK=…`
      (`-Wall -Werror` — a warning is an error).
- [ ] If `webui/src/` changed: I re-ran `npm run build:embed` in `webui/`
      and committed the refreshed `payload/webui.html`; `RECEIVER_BUILD`
      in `payload/common.h` is bumped if the page's behavior changed.
- [ ] Commit messages follow the repo style, e.g.
      `fix(installer): say 0x80B21104 needs room for two copies`.
- [ ] Docs are updated wherever the change affects users.
