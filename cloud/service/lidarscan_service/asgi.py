"""`uvicorn lidarscan_service.asgi:app` — configuration comes from the env.

A missing or invalid setting raises here, at import, which is exactly when a
process manager should see it.
"""

from __future__ import annotations

from .api import create_app
from .config import Config, describe
from .main import configure_logging, log

configure_logging()
_cfg = Config.from_env()
for _line in describe(_cfg):
    log.info("config: %s", _line)

app = create_app(_cfg)
