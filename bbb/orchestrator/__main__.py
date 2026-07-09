"""`python -m orchestrator` entrypoint."""
import asyncio
import sys

from orchestrator.agent import main

if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
