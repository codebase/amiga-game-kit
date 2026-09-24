import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
THIRD_PARTY = os.path.join(ROOT, "third_party")
VAMIGA = os.environ.get("AGK_VAMIGA", os.path.join(THIRD_PARTY, "build-vamiga/VAHeadless"))
CACHE = os.path.join(ROOT, "out", ".cache")
