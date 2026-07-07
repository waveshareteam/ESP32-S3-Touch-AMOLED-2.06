#!/usr/bin/env python3
"""Download and extract firmware artifacts from a GitHub Actions run."""

from __future__ import print_function

import os
import sys

_MIN_PYTHON = (3, 10)
_REEXEC_ENV = "WAVESHARE_DOWNLOAD_ARTIFACTS_REEXEC"


def _implementation_path():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "download_artifacts_impl.py")


def _exec_python3():
    python3 = os.environ.get("PYTHON3", "python3")
    os.environ[_REEXEC_ENV] = "1"
    os.execvp(python3, [python3, os.path.abspath(__file__)] + sys.argv[1:])


if sys.version_info < _MIN_PYTHON:
    if os.environ.get(_REEXEC_ENV) != "1":
        try:
            _exec_python3()
        except OSError:
            pass
    sys.stderr.write("error: this script requires Python 3.10 or newer. Run it with `python3`.\n")
    raise SystemExit(1)

from download_artifacts_impl import main


if __name__ == "__main__":
    raise SystemExit(main())
