"""``python -m 米.浏览器`` — open the Python edition of Mi Browser."""
from __future__ import annotations

import sys

from .tk_app import main

if __name__ == "__main__":
    sys.exit(main())
