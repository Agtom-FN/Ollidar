"""CLI entry point: `python -m lidarscan_service`.

For a process manager that wants uvicorn itself, use the ASGI module instead:

    uvicorn lidarscan_service.asgi:app --host 127.0.0.1 --port 8080

Binds 127.0.0.1 by default. Put TLS in front of it (README "Deploy") — this
service speaks plain HTTP on purpose and has no business on 0.0.0.0.
"""

from __future__ import annotations

import logging
import sys

log = logging.getLogger("lidarscan")


def configure_logging() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )


def main(argv: list[str] | None = None) -> int:
    import uvicorn

    from .api import create_app
    from .config import Config, ConfigError, describe

    configure_logging()
    try:
        cfg = Config.from_env()
    except ConfigError as exc:
        print(f"lidarscan-service: {exc}", file=sys.stderr)
        return 2
    for line in describe(cfg):
        log.info("config: %s", line)
    uvicorn.run(create_app(cfg), host=cfg.host, port=cfg.port, log_level="info")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
